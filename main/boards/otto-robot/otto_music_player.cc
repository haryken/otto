#include "otto_music_player.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <board.h>
#include <http.h>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "device_state.h"
#include "protocol.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_dec.h"
#include "esp_audio_types.h"

#define TAG "OttoMusic"

static constexpr const char* kYoutubeApiHost = "https://youtube.kytuoi.com";
static constexpr int kHttpTimeoutMs = 120000;
static constexpr size_t kMp3ReadChunk = 8192;
static constexpr size_t kPcmDecodeBuf = 8192;
/** HTTP connect id (1 is often used by WebSocket). */
static constexpr int kMusicHttpConnectId = 2;
/**
 * Decode budget per outer loop. Higher = fewer underruns (stutter), still yields
 * so Wi-Fi + wake word can run. Wake word stays enabled during playback.
 */
static constexpr int kMaxMp3FramesPerLoop = 12;
static constexpr int kMaxMp3DecodeIterations = 28;
/** Pause HTTP read when carry buffer exceeds this (decode must catch up). */
static constexpr size_t kMp3BufferPauseRead = 48 * 1024;
/** Prefill MP3 before first PCM enqueue — hides early network jitter (keep small: low SRAM). */
static constexpr size_t kMp3PrebufferBytes = 12 * 1024;
/** MP3 frame can be large; decoding with less causes DATA_LACK / NOT_SUPPORT spam. */
static constexpr size_t kMinMp3BytesBeforeDecode = 2048;
/** otto_music task priority (wake word / AFE stay higher or equal elsewhere). */
static constexpr UBaseType_t kMusicTaskPriority = 5;

static std::atomic<bool> g_stop_requested{false};
static std::atomic<bool> g_is_playing{false};
static TaskHandle_t g_play_task = nullptr;

static std::string UrlEncodeQuery(const std::string& input) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('%');
            out.push_back('2');
            out.push_back('0');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

/** Stricter than 0xFF 0xE* — avoids false sync inside compressed data (NOT_SUPPORT spam). */
static bool IsValidMp3FrameHeader(const uint8_t* data, size_t len, size_t offset) {
    if (offset + 4 > len) {
        return false;
    }
    if (data[offset] != 0xFF) {
        return false;
    }
    const uint8_t h1 = data[offset + 1];
    if ((h1 & 0xE0) != 0xE0) {
        return false;
    }
    const uint8_t version = (h1 >> 3) & 3;
    const uint8_t layer = (h1 >> 1) & 3;
    if (version == 1 || layer != 1) {
        return false;
    }
    const uint8_t h2 = data[offset + 2];
    if (((h2 >> 4) & 0x0F) == 0x0F || ((h2 >> 2) & 3) == 3) {
        return false;
    }
    return true;
}

static size_t FindNextValidMp3Frame(const uint8_t* data, size_t len, size_t start) {
    const size_t begin = start + 1;
    if (begin + 4 > len) {
        return len;
    }
    for (size_t i = begin; i + 4 <= len; ++i) {
        if (IsValidMp3FrameHeader(data, len, i)) {
            return i;
        }
    }
    return len;
}

static size_t Id3v2TagSize(const uint8_t* data) {
    return ((static_cast<size_t>(data[6]) & 0x7F) << 21) | ((static_cast<size_t>(data[7]) & 0x7F) << 14) |
           ((static_cast<size_t>(data[8]) & 0x7F) << 7) | (static_cast<size_t>(data[9]) & 0x7F);
}

/**
 * Find byte offset where MP3 frames start. Returns true when buffer is ready to decode.
 * @param skip_out bytes to strip before decode (0 = sync already at index 0).
 */
static bool FindMp3PayloadOffset(const uint8_t* data, size_t len, size_t& skip_out) {
    skip_out = 0;
    if (len < 2) {
        return false;
    }
    if (len >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        const size_t tag_end = 10 + Id3v2TagSize(data);
        if (tag_end <= len) {
            skip_out = tag_end;
            return true;
        }
        for (size_t i = 10; i + 4 <= len; ++i) {
            if (IsValidMp3FrameHeader(data, len, i)) {
                skip_out = i;
                return true;
            }
        }
        return false;
    }
    for (size_t i = 0; i + 4 <= len; ++i) {
        if (IsValidMp3FrameHeader(data, len, i)) {
            skip_out = i;
            return true;
        }
    }
    return false;
}

static bool HttpGetBody(const std::string& url, std::string& body, int& status_code) {
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    ESP_LOGI(TAG, "HTTP GET begin: %s", url.c_str());
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kMusicHttpConnectId);
    if (!http) {
        ESP_LOGE(TAG, "HTTP CreateHttp failed (connect_id=%d)", kMusicHttpConnectId);
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "*/*");
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "HTTP open failed: %s (err %d)", url.c_str(), http->GetLastError());
        return false;
    }
    status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP %d for %s", status_code, url.c_str());
        http->Close();
        return false;
    }
    body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "HTTP GET ok: status=%d body_len=%u", status_code,
             static_cast<unsigned>(body.size()));
    return true;
}

struct SearchResult {
    std::string id;
    std::string title;
};

static bool YoutubeSearchFirst(const std::string& query, SearchResult& out) {
    const std::string url =
        std::string(kYoutubeApiHost) + "/api/search?q=" + UrlEncodeQuery(query) + "&limit=1";
    ESP_LOGI(TAG, "Search start query=\"%s\"", query.c_str());
    std::string body;
    int status = 0;
    if (!HttpGetBody(url, body, status)) {
        ESP_LOGE(TAG, "Search HTTP failed query=\"%s\"", query.c_str());
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Search JSON parse fail body_len=%u head=\"%.80s\"",
                 static_cast<unsigned>(body.size()), body.c_str());
        return false;
    }
    bool ok = false;
    cJSON* success = cJSON_GetObjectItem(root, "success");
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsTrue(success) && cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
        cJSON* first = cJSON_GetArrayItem(data, 0);
        cJSON* id = cJSON_GetObjectItem(first, "id");
        cJSON* title = cJSON_GetObjectItem(first, "title");
        if (cJSON_IsString(id) && cJSON_IsString(title)) {
            out.id = id->valuestring;
            out.title = title->valuestring;
            ok = true;
            ESP_LOGI(TAG, "Search hit id=%s title=\"%s\"", out.id.c_str(), out.title.c_str());
        }
    }
    if (!ok) {
        ESP_LOGE(TAG, "Search empty/no hit body_len=%u head=\"%.120s\"",
                 static_cast<unsigned>(body.size()), body.c_str());
    }
    cJSON_Delete(root);
    return ok;
}

static void DownmixToMono(std::vector<int16_t>& pcm, int channels) {
    if (channels <= 1) {
        return;
    }
    const size_t frames = pcm.size() / channels;
    for (size_t i = 0; i < frames; ++i) {
        int32_t sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
            sum += pcm[i * channels + ch];
        }
        pcm[i] = static_cast<int16_t>(sum / channels);
    }
    pcm.resize(frames);
}

/** Pause voice uplink during MP3; keep wake word on so user can interrupt music. */
static void SuspendCaptureForMusic(Application& app) {
    app.GetAudioService().EnableVoiceProcessing(false);
}

static void RestoreCaptureAfterMusic(Application& app) {
    app.ResetMusicOnlySession();
    app.ClearMusicPlayingOnDisplay();
    auto& audio = app.GetAudioService();
    audio.RestoreAudioHardwareAfterLocalPlayback();

    const DeviceState state = app.GetDeviceState();
    if (state == kDeviceStateListening) {
        app.Schedule([&app]() { app.FinishListeningMicSetup(); });
    }
    switch (state) {
        case kDeviceStateIdle:
            audio.EnableWakeWordDetection(true);
            break;
        case kDeviceStateListening:
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
#else
            audio.EnableWakeWordDetection(false);
#endif
            break;
        case kDeviceStateSpeaking:
            audio.EnableWakeWordDetection(audio.IsAfeWakeWord());
            break;
        default:
            break;
    }
}

static void ScheduleRestoreAfterMusic(Application& app) {
    app.Schedule([&app]() {
        // Allow normal idle power-save again after local stream ends.
        RestoreCaptureAfterMusic(app);
    });
}

static void FailMusicBeforePlay(Application& app) {
    g_is_playing.store(false);
    g_play_task = nullptr;
    app.NotifyMusicSearchFailed();
}

static void StreamMp3Task(void* param);

/** Search YouTube then stream — runs off the main thread so TTS UDP is not starved. */
static void SearchAndPlayTask(void* param) {
    auto& app = Application::GetInstance();
    std::unique_ptr<std::string> query(static_cast<std::string*>(param));
    ESP_LOGI(TAG, "SearchAndPlayTask enter query=\"%s\" stop=%d playing=%d", query->c_str(),
             g_stop_requested.load() ? 1 : 0, g_is_playing.load() ? 1 : 0);
    app.ShowMusicSearchingOnDisplay();
    SearchResult result;
    if (!YoutubeSearchFirst(*query, result)) {
        ESP_LOGE(TAG, "Search failed: %s", query->c_str());
        FailMusicBeforePlay(app);
        vTaskDelete(nullptr);
        return;
    }
    if (g_stop_requested.load()) {
        ESP_LOGW(TAG, "SearchAndPlayTask aborted after search (stop requested)");
        g_is_playing.store(false);
        g_play_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    g_stop_requested.store(false);
    g_is_playing.store(true);
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetCaptureSuspended(true);
    audio.SetLocalPlaybackActive(true);
    audio.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "Hand off to StreamMp3Task id=%s", result.id.c_str());

    auto* track = new SearchResult(std::move(result));
    StreamMp3Task(track);
}

static void StreamMp3Task(void* param) {
    std::unique_ptr<SearchResult> track(static_cast<SearchResult*>(param));
    g_is_playing.store(true);
    g_stop_requested.store(false);
    ESP_LOGI(TAG, "StreamMp3Task enter id=%s title=\"%s\"", track->id.c_str(), track->title.c_str());

    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    // Music-only closes MQTT and may drop WiFi to LOW_POWER — force PERFORMANCE for stream.
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio.SetCaptureSuspended(true);
    audio.SetLocalPlaybackActive(true);
    // Keep wake word on so user can interrupt with Hi,Jason / BOOT.
    audio.EnableWakeWordDetection(true);
    SuspendCaptureForMusic(app);
    audio.ResetDecoder();

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    // format=stream returns fragmented MP4 (ftyp dash) — NOT MP3. format=mp3 returns audio/mpeg.
    const std::string stream_url = std::string(kYoutubeApiHost) + "/api/stream/mp3?id=" + track->id +
                                   "&format=mp3";
    ESP_LOGI(TAG, "Stream URL: %s", stream_url.c_str());

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(kMusicHttpConnectId);
    if (!http) {
        ESP_LOGE(TAG, "Stream CreateHttp failed (connect_id=%d)", kMusicHttpConnectId);
        FailMusicBeforePlay(app);
        vTaskDelete(nullptr);
        return;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "audio/mpeg,*/*");

    if (!http->Open("GET", stream_url)) {
        ESP_LOGE(TAG, "Stream open failed (err %d)", http->GetLastError());
        http->Close();
        FailMusicBeforePlay(app);
        vTaskDelete(nullptr);
        return;
    }
    if (http->GetStatusCode() != 200 && http->GetStatusCode() != 206) {
        ESP_LOGE(TAG, "Stream HTTP %d", http->GetStatusCode());
        http->Close();
        FailMusicBeforePlay(app);
        vTaskDelete(nullptr);
        return;
    }

    esp_mp3_dec_register();
    void* mp3_dec = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &mp3_dec) != ESP_AUDIO_ERR_OK || mp3_dec == nullptr) {
        ESP_LOGE(TAG, "MP3 decoder open failed");
        http->Close();
        FailMusicBeforePlay(app);
        vTaskDelete(nullptr);
        return;
    }

    std::vector<uint8_t> read_buf(kMp3ReadChunk);
    std::vector<uint8_t> pcm_buf(kPcmDecodeBuf);
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int src_rate = 0;
    int src_channels = 1;

    auto cleanup = [&](bool natural_end) {
        if (resampler) {
            esp_ae_rate_cvt_close(resampler);
            resampler = nullptr;
        }
        if (mp3_dec) {
            esp_mp3_dec_close(mp3_dec);
            mp3_dec = nullptr;
        }
        http->Close();
        g_is_playing.store(false);
        g_play_task = nullptr;
        if (natural_end) {
            app.NotifyMusicFinished();
        } else {
            ScheduleRestoreAfterMusic(app);
        }
    };

    const std::string content_type = http->GetResponseHeader("Content-Type");
    ESP_LOGI(TAG, "Streaming: %s (%s) HTTP %d", track->title.c_str(), content_type.c_str(),
             http->GetStatusCode());

    app.EnterMusicOnlyMode();
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    app.ShowMusicPlayingOnDisplay();

    if (g_stop_requested.load()) {
        ESP_LOGW(TAG, "Playback cancelled before read");
        cleanup(false);
        vTaskDelete(nullptr);
        return;
    }

    std::vector<uint8_t> mp3_carry;
    bool id3_skipped = false;
    bool prebuffer_done = false;
    bool stream_eof = false;

    while (!g_stop_requested.load()) {
        if (g_stop_requested.load()) {
            break;
        }
        int n = 0;
        if (!stream_eof && mp3_carry.size() < kMp3BufferPauseRead) {
            n = http->Read(reinterpret_cast<char*>(read_buf.data()), read_buf.size());
            if (n > 0) {
                mp3_carry.insert(mp3_carry.end(), read_buf.data(), read_buf.data() + n);
            } else {
                // EOF/error: exit after draining carry — leftover bytes used to spin forever
                // with "Đang phát nhạc" and never NotifyMusicFinished.
                stream_eof = true;
                ESP_LOGI(TAG, "MP3 HTTP EOF (read=%d), carry=%u bytes", n,
                         static_cast<unsigned>(mp3_carry.size()));
            }
        }
        if (stream_eof && mp3_carry.empty()) {
            ESP_LOGI(TAG, "MP3 stream drained");
            break;
        }
        if (!id3_skipped) {
            size_t skip = 0;
            if (!FindMp3PayloadOffset(mp3_carry.data(), mp3_carry.size(), skip)) {
                if (stream_eof || mp3_carry.size() > 256 * 1024) {
                    ESP_LOGE(TAG, "No MP3 frame sync in stream (wrong format?)");
                    break;
                }
                continue;
            }
            if (skip > 0) {
                if (skip >= mp3_carry.size()) {
                    mp3_carry.clear();
                    continue;
                }
                mp3_carry.erase(mp3_carry.begin(), mp3_carry.begin() + static_cast<std::ptrdiff_t>(skip));
                if (mp3_carry.empty()) {
                    continue;
                }
            }
            id3_skipped = true;
            ESP_LOGI(TAG, "MP3 payload at offset %u, buffer %u bytes", static_cast<unsigned>(skip),
                     static_cast<unsigned>(mp3_carry.size()));
        }

        if (!prebuffer_done) {
            if (!stream_eof && mp3_carry.size() < kMp3PrebufferBytes && n > 0) {
                continue;
            }
            prebuffer_done = true;
            ESP_LOGI(TAG, "Prebuffer ready: %u bytes (wake word still on)",
                     static_cast<unsigned>(mp3_carry.size()));
        }

        if (id3_skipped && mp3_carry.size() < kMinMp3BytesBeforeDecode) {
            if (stream_eof) {
                ESP_LOGW(TAG, "EOF with incomplete MP3 tail (%u bytes), ending",
                         static_cast<unsigned>(mp3_carry.size()));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t mp3_pos = 0;
        int frames_this_loop = 0;
        int decode_iterations = 0;
        int max_frames = kMaxMp3FramesPerLoop;
        int max_iters = kMaxMp3DecodeIterations;
        if (mp3_carry.size() > 32 * 1024) {
            max_frames = 16;
            max_iters = 36;
        }
        if (mp3_carry.size() > 64 * 1024) {
            max_frames = 24;
            max_iters = 48;
        }
        while (mp3_pos < mp3_carry.size() && !g_stop_requested.load() && frames_this_loop < max_frames &&
               decode_iterations < max_iters) {
            decode_iterations++;
            esp_audio_dec_in_raw_t raw = {
                .buffer = mp3_carry.data() + mp3_pos,
                .len = static_cast<uint32_t>(mp3_carry.size() - mp3_pos),
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            esp_audio_dec_out_frame_t out_frame = {
                .buffer = pcm_buf.data(),
                .len = static_cast<uint32_t>(pcm_buf.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_dec_info_t dec_info = {};
            esp_audio_err_t ret = esp_mp3_dec_decode(mp3_dec, &raw, &out_frame, &dec_info);

            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                pcm_buf.resize(out_frame.needed_size);
                continue;
            }
            if (ret == ESP_AUDIO_ERR_DATA_LACK) {
                break;
            }
            if (ret == ESP_AUDIO_ERR_NOT_SUPPORT || ret == ESP_AUDIO_ERR_FAIL) {
                if (raw.consumed > 0) {
                    mp3_pos += raw.consumed;
                    continue;
                }
                const size_t next = FindNextValidMp3Frame(mp3_carry.data(), mp3_carry.size(), mp3_pos);
                if (next + 4 <= mp3_carry.size() && next > mp3_pos) {
                    static int resync_log_count = 0;
                    if (++resync_log_count <= 2) {
                        ESP_LOGD(TAG, "MP3 resync: skip %u bytes to next frame",
                                 static_cast<unsigned>(next - mp3_pos));
                    }
                    mp3_pos = next;
                    continue;
                }
                break;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                break;
            }

            mp3_pos += raw.consumed;

            if (out_frame.decoded_size == 0) {
                continue;
            }

            if (src_rate == 0 && dec_info.sample_rate > 0) {
                src_rate = static_cast<int>(dec_info.sample_rate);
                src_channels = dec_info.channel > 0 ? dec_info.channel : 1;
            }

            const size_t samples = out_frame.decoded_size / sizeof(int16_t);
            std::vector<int16_t> pcm(samples);
            memcpy(pcm.data(), pcm_buf.data(), out_frame.decoded_size);

            if (src_channels > 1) {
                DownmixToMono(pcm, src_channels);
            }

            if (resampler == nullptr && src_rate > 0 && src_rate != codec->output_sample_rate()) {
                esp_ae_rate_cvt_cfg_t cfg = {
                    .src_rate = static_cast<uint32_t>(src_rate),
                    .dest_rate = static_cast<uint32_t>(codec->output_sample_rate()),
                    .channel = 1,
                    .bits_per_sample = ESP_AUDIO_BIT16,
                    .complexity = 2,
                    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                };
                if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK) {
                    ESP_LOGW(TAG, "Resampler open failed");
                    resampler = nullptr;
                }
            }

            if (resampler) {
                uint32_t out_samples = 0;
                esp_ae_rate_cvt_get_max_out_sample_num(resampler, pcm.size(), &out_samples);
                std::vector<int16_t> resampled(out_samples);
                uint32_t actual = out_samples;
                esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t*>(pcm.data()),
                                        pcm.size(), reinterpret_cast<esp_ae_sample_t*>(resampled.data()),
                                        &actual);
                resampled.resize(actual);
                pcm = std::move(resampled);
            }

            if (!g_stop_requested.load()) {
                audio.EnqueuePlaybackPcm(std::move(pcm));
                frames_this_loop++;
            }
        }

        if (mp3_pos > 0 && mp3_pos <= mp3_carry.size()) {
            mp3_carry.erase(mp3_carry.begin(), mp3_carry.begin() + static_cast<std::ptrdiff_t>(mp3_pos));
        }

        if (stream_eof) {
            if (mp3_carry.empty() || frames_this_loop == 0) {
                if (!mp3_carry.empty()) {
                    ESP_LOGW(TAG, "EOF dropping %u leftover MP3 bytes",
                             static_cast<unsigned>(mp3_carry.size()));
                    mp3_carry.clear();
                }
                break;
            }
        }

        if (mp3_carry.size() >= kMp3BufferPauseRead) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (stream_eof) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            taskYIELD();
        }
    }

    // Let speaker finish queued PCM before "hết nhạc" + wake.
    if (!g_stop_requested.load()) {
        audio.WaitForPlaybackQueueEmpty();
    }

    const bool natural_end = !g_stop_requested.load();
    cleanup(natural_end);
    ESP_LOGI(TAG, "Playback finished: %s (natural=%d)", track->title.c_str(), natural_end ? 1 : 0);
    vTaskDelete(nullptr);
}

namespace OttoMusic {

bool IsPlaying() { return g_is_playing.load(); }

void Stop() {
    if (!g_is_playing.load() && g_play_task == nullptr) {
        ScheduleRestoreAfterMusic(Application::GetInstance());
        return;
    }

    ESP_LOGI(TAG, "Stopping music for voice interaction");
    g_stop_requested.store(true);
    Application::GetInstance().GetAudioService().ResetDecoder();

    for (int i = 0; i < 50 && g_is_playing.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!g_is_playing.load()) {
        ScheduleRestoreAfterMusic(Application::GetInstance());
    } else {
        ESP_LOGW(TAG, "Music task still exiting");
    }
}

std::string PlayFirstSearchResult(const std::string& query) {
    ESP_LOGI(TAG, "PlayFirstSearchResult called query=\"%s\" was_playing=%d task=%p",
             query.c_str(), g_is_playing.load() ? 1 : 0, static_cast<void*>(g_play_task));
    if (query.empty()) {
        ESP_LOGE(TAG, "PlayFirstSearchResult: empty query");
        Application::GetInstance().NotifyMusicSearchFailed();
        return R"({"success":false,"error":"query is empty"})";
    }

    g_stop_requested.store(true);
    for (int i = 0; i < 50 && g_is_playing.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    g_stop_requested.store(false);

    Application::GetInstance().ShowMusicSearchingOnDisplay();

    auto* query_copy = new std::string(query);
    BaseType_t created = xTaskCreatePinnedToCore(
        SearchAndPlayTask, "otto_music", 8192 * 3, query_copy, kMusicTaskPriority, &g_play_task, 0);
    if (created != pdPASS) {
        delete query_copy;
        g_play_task = nullptr;
        ESP_LOGE(TAG, "PlayFirstSearchResult: xTaskCreate failed");
        Application::GetInstance().NotifyMusicSearchFailed();
        return R"({"success":false,"error":"failed to start playback task"})";
    }

    ESP_LOGI(TAG, "PlayFirstSearchResult: otto_music task started handle=%p",
             static_cast<void*>(g_play_task));

    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    cJSON_AddStringToObject(json, "query", query.c_str());
    cJSON_AddStringToObject(json, "status", "starting");
    char* str = cJSON_PrintUnformatted(json);
    std::string out = str ? str : R"({"success":false,"error":"json"})";
    if (str) {
        cJSON_free(str);
    }
    cJSON_Delete(json);
    return out;
}

}  // namespace OttoMusic

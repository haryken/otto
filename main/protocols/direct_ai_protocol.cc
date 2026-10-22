#include "direct_ai_protocol.h"
#include "application.h"
#include "board.h"
#include "audio_codec.h"
#include "display.h"
#include "mcp_server.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <mbedtls/base64.h>

#define TAG "DirectAI"

namespace {

constexpr int kRealtimeRate = 24000;
constexpr int kMicRate = 16000;
constexpr int kSilenceMs = 700;
constexpr int kSetupTimeoutMs = 10000;
constexpr int kPlaybackChunkMs = 60;
constexpr int64_t kGateWaitUs = 3000LL * 1000;
// Closes a forgotten session (billing runs while the socket is open).
constexpr int64_t kIdleCloseUs = 90LL * 1000 * 1000;
// After a tool response Gemini usually keeps talking in the same turn.
constexpr int64_t kToolGraceUs = 4000LL * 1000;
constexpr size_t kInboxMaxBytes = 6 * 1024 * 1024;
constexpr size_t kMaxFrameBytes = 65535;  // WebSocket::Send limit

constexpr EventBits_t kReadyBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;

constexpr const char* kEndTool = "self.conversation.end";
// Application::SendSilencePromptText sends this after 30 s of silence.
constexpr const char* kSilencePromptText = "khơi gợi lại";
constexpr const char* kNudgeText =
    "(Người dùng im lặng nãy giờ. Hãy hỏi han hoặc gợi chuyện bằng một câu thật ngắn.)";

constexpr const char* kBaseInstructions =
    "Bạn là chú robot nhỏ thông minh, tinh nghịch và tốt bụng của người dùng.\n"
    "Mặc định trả lời bằng tiếng Việt; nếu người dùng nói ngôn ngữ khác thì trả lời bằng ngôn ngữ đó.\n"
    "Câu trả lời sẽ được đọc thành tiếng: nói tự nhiên, ngắn gọn (thường 1-3 câu), không emoji, "
    "không markdown, không gạch đầu dòng.\n"
    "Bạn có các công cụ (function) điều khiển thân robot và đọc thông tin robot. Khi người dùng muốn "
    "robot LÀM gì đó (đi, nhảy, vẫy tay, đổi âm lượng, độ sáng, kiểm tra pin, ...), hãy hiểu theo ý "
    "nghĩa câu nói chứ không cần đúng từ khóa, BẮT BUỘC gọi công cụ phù hợp, rồi nói một câu xác nhận "
    "ngắn. Không được nói là đã làm nếu chưa gọi công cụ.\n"
    "Không bịa thông tin mà công cụ có thể cung cấp (pin, trạng thái, âm lượng).\n"
    "Khi người dùng chào tạm biệt hoặc muốn dừng nói chuyện, nói lời tạm biệt ngắn và gọi công cụ "
    "self__conversation__end.";

constexpr const char* kEndToolDescription =
    "Call when the user says goodbye or wants to stop talking (tạm biệt, bye, thôi nhé, dừng nói "
    "chuyện, không nói nữa, đi ngủ đi). Say a short goodbye first; the robot then stops listening "
    "until the wake word.";

constexpr const char* kOpenAIUrl = "wss://api.openai.com/v1/realtime?model=";
constexpr const char* kGrokUrl = "wss://api.x.ai/v1/realtime?model=";
constexpr const char* kGeminiUrl =
    "wss://generativelanguage.googleapis.com/ws/"
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";

int64_t NowUs() {
    return esp_timer_get_time();
}

bool IsRealtime(DirectAiBackend backend) {
    return backend == DirectAiBackend::kOpenAI || backend == DirectAiBackend::kGrok;
}

bool IsBetaModel(const std::string& model) {
    return model.find("preview") != std::string::npos;
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// OpenAI function names must match ^[a-zA-Z0-9_-]{1,64}$; "." travels as "__".
std::string WireToolName(const std::string& mcp_name) {
    return ReplaceAll(mcp_name, ".", "__");
}

std::string McpToolName(const std::string& wire_name) {
    return ReplaceAll(wire_name, "__", ".");
}

std::string Base64Encode(const void* data, size_t len) {
    std::string out(4 * ((len + 2) / 3) + 1, '\0');
    size_t olen = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &olen,
                          static_cast<const unsigned char*>(data), len);
    out.resize(olen);
    return out;
}

bool Base64Decode(const char* src, size_t len, std::vector<uint8_t>& out) {
    out.resize(len / 4 * 3 + 3);
    size_t olen = 0;
    if (mbedtls_base64_decode(out.data(), out.size(), &olen,
                              reinterpret_cast<const unsigned char*>(src), len) != 0) {
        return false;
    }
    out.resize(olen);
    return true;
}

const char* JsonString(const cJSON* obj, const char* key) {
    auto item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

std::string PrintJson(const cJSON* root) {
    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed ? printed : "";
    cJSON_free(printed);
    return out;
}

// close_reason() exists only in the patched esp-ml307 under components/ (gitignored).
template <typename Ws>
std::string CloseReason(const Ws& ws) {
    if constexpr (requires { ws.close_reason(); }) {
        return ws.close_reason();
    } else {
        return {};
    }
}

int RateFromMime(const char* mime, int fallback) {
    const char* p = mime ? strstr(mime, "rate=") : nullptr;
    int rate = p ? atoi(p + 5) : 0;
    return rate > 0 ? rate : fallback;
}

esp_ae_rate_cvt_handle_t OpenResampler(int src_rate, int dest_rate) {
    esp_ae_rate_cvt_cfg_t cfg = {
        .src_rate = static_cast<uint32_t>(src_rate),
        .dest_rate = static_cast<uint32_t>(dest_rate),
        .channel = 1,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    esp_ae_rate_cvt_handle_t handle = nullptr;
    if (esp_ae_rate_cvt_open(&cfg, &handle) != ESP_AE_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open resampler %d -> %d", src_rate, dest_rate);
        return nullptr;
    }
    return handle;
}

std::vector<int16_t> Resample(esp_ae_rate_cvt_handle_t handle, std::vector<int16_t>& in) {
    uint32_t max_out = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(handle, in.size(), &max_out);
    std::vector<int16_t> out(max_out);
    uint32_t produced = max_out;
    esp_ae_rate_cvt_process(handle, reinterpret_cast<esp_ae_sample_t>(in.data()), in.size(),
                            reinterpret_cast<esp_ae_sample_t>(out.data()), &produced);
    out.resize(produced);
    return out;
}

// Gemini accepts only this JSON Schema subset in function parameters.
bool GeminiSchemaKey(const char* key) {
    static const char* const kKeys[] = {
        "type", "description", "enum", "properties", "required", "items",
        "minimum", "maximum", "format", "nullable",
    };
    for (auto k : kKeys) {
        if (strcmp(k, key) == 0) {
            return true;
        }
    }
    return false;
}

cJSON* GeminiSchema(const cJSON* in) {
    cJSON* out = cJSON_CreateObject();
    const cJSON* child = nullptr;
    cJSON_ArrayForEach(child, in) {
        if (child->string == nullptr || !GeminiSchemaKey(child->string)) {
            continue;
        }
        if (strcmp(child->string, "properties") == 0) {
            cJSON* props = cJSON_CreateObject();
            const cJSON* prop = nullptr;
            cJSON_ArrayForEach(prop, child) {
                cJSON_AddItemToObject(props, prop->string, GeminiSchema(prop));
            }
            cJSON_AddItemToObject(out, "properties", props);
        } else if (strcmp(child->string, "items") == 0) {
            cJSON_AddItemToObject(out, "items", GeminiSchema(child));
        } else {
            cJSON_AddItemToObject(out, child->string, cJSON_Duplicate(child, true));
        }
    }
    return out;
}

}  // namespace

DirectAiProtocol::DirectAiProtocol() {
    events_ = xEventGroupCreate();
    worker_done_ = xSemaphoreCreateBinary();

    codec_rate_ = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    server_sample_rate_ = codec_rate_;
    up_resampler_ = OpenResampler(kMicRate, kRealtimeRate);

    Application::GetInstance().GetAudioService().SetRawPcmUplink(true);

    // The worker never touches flash, so its stack can live in PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps([](void* arg) {
            static_cast<DirectAiProtocol*>(arg)->WorkerLoop();
        }, "direct_ai", 8192, this, 4, &worker_, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        worker_ = nullptr;
        ESP_LOGE(TAG, "Failed to start worker task");
    }
}

DirectAiProtocol::~DirectAiProtocol() {
    alive_.reset();
    muted_ = true;
    ready_ = false;
    CloseSocket();
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        stop_ = true;
    }
    inbox_cv_.notify_all();
    // Unblocks a worker waiting for playback queue space.
    Application::GetInstance().GetAudioService().ResetDecoder();
    if (worker_ != nullptr) {
        xSemaphoreTake(worker_done_, portMAX_DELAY);
        vTaskDeleteWithCaps(worker_);
    }
    vSemaphoreDelete(worker_done_);
    if (up_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(up_resampler_);
    }
    if (down_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(down_resampler_);
    }
    vEventGroupDelete(events_);
    Application::GetInstance().GetAudioService().SetRawPcmUplink(false);
}

bool DirectAiProtocol::Start() {
    // Connect only when a conversation starts.
    return true;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

bool DirectAiProtocol::OpenAudioChannel() {
    CloseSocket();
    config_ = DirectAiConfig::Load();
    backend_ = config_.backend;
    config_generation_ = DirectAiConfig::Generation();
    error_occurred_ = false;

    const char* name = DirectAiConfig::BackendName(config_.backend);
    if (config_.backend == DirectAiBackend::kOff) {
        SetError("AI: chưa chọn nhà cung cấp (web :8080 — dạy học: Trang chủ; World Edu: tab AI)");
        return false;
    }
    if (config_.key.empty()) {
        SetError(std::string("AI: chưa nhập API key ") + name + " (web :8080, tab AI)");
        return false;
    }

    const int idx = static_cast<int>(config_.backend);
    bool search = config_.search && config_.backend != DirectAiBackend::kOpenAI && !no_search_[idx];
    std::string error;
    bool ok = Connect(search, &error);
    if (!ok && search) {
        // Keys without search quota/permission refuse the whole session.
        ESP_LOGW(TAG, "%s refused the session with web search (%s), retrying without it",
                 name, error.c_str());
        // Let TLS/WebSocket teardown finish before the next malloc-heavy setup JSON.
        vTaskDelay(pdMS_TO_TICKS(300));
        ok = Connect(false, &error);
        if (ok) {
            no_search_[idx] = true;
        }
    }
    if (!ok) {
        ESP_LOGE(TAG, "%s session failed: %s", name, error.c_str());
        SetError("AI: " + error);
        return false;
    }

    if (up_resampler_ != nullptr) {
        esp_ae_rate_cvt_reset(up_resampler_);
    }
    appended_ms_ = 0;
    silence_prompts_ = 0;
    last_activity_us_ = NowUs();
    last_incoming_time_ = std::chrono::steady_clock::now();
    ready_ = true;
    ESP_LOGI(TAG, "%s session ready (model %s, voice %s)", name, config_.model.c_str(), config_.voice.c_str());
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

bool DirectAiProtocol::Connect(bool search, std::string* error) {
    CloseSocket();
    const uint32_t conn = ++conn_id_;
    xEventGroupClearBits(events_, kReadyBit | kFailedBit);
    {
        std::lock_guard<std::mutex> lock(setup_mutex_);
        setup_error_.clear();
    }

    auto ws = Board::GetInstance().GetNetwork()->CreateWebSocket(1);
    if (ws == nullptr) {
        *error = "không tạo được kết nối";
        return false;
    }

    std::string url;
    std::string setup;
    switch (config_.backend) {
        case DirectAiBackend::kOpenAI: {
            const bool beta = IsBetaModel(config_.model);
            url = kOpenAIUrl + config_.model;
            ws->SetHeader("Authorization", ("Bearer " + config_.key).c_str());
            if (beta) {
                ws->SetHeader("OpenAI-Beta", "realtime=v1");
            }
            setup = BuildOpenAISetup(beta);
            break;
        }
        case DirectAiBackend::kGrok:
            url = kGrokUrl + config_.model;
            ws->SetHeader("Authorization", ("Bearer " + config_.key).c_str());
            setup = BuildGrokSetup(search);
            break;
        case DirectAiBackend::kGemini:
            url = kGeminiUrl;
            ws->SetHeader("x-goog-api-key", config_.key.c_str());
            setup = BuildGeminiSetup(search);
            break;
        default:
            *error = "chưa chọn nhà cung cấp";
            return false;
    }
    if (setup.size() > kMaxFrameBytes) {
        *error = "cấu hình phiên quá lớn";
        return false;
    }

    ws->OnData([this, conn](const char* data, size_t len, bool binary) {
        Push(InboxItem{kInboxServer, conn, std::string(data, len)});
    });
    ws->OnDisconnected([this, conn]() {
        OnSocketClosed(conn);
    });

    WebSocket* socket = ws.get();
    {
        std::lock_guard<std::mutex> lock(ws_mutex_);
        ws_ = std::move(ws);
    }

    ESP_LOGI(TAG, "Connecting %s%s", url.c_str(), search ? " (web search on)" : "");
    if (!socket->Connect(url.c_str())) {
        *error = CloseReason(*socket);
        if (error->empty()) {
            *error = "không kết nối được máy chủ";
        }
        CloseSocket();
        return false;
    }
    if (!Send(setup)) {
        *error = "không gửi được cấu hình phiên";
        CloseSocket();
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(events_, kReadyBit | kFailedBit, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(kSetupTimeoutMs));
    if (bits & kReadyBit) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(setup_mutex_);
        *error = setup_error_;
    }
    if (error->empty()) {
        *error = CloseReason(*socket);
    }
    if (error->empty()) {
        *error = (bits & kFailedBit) ? "máy chủ đóng kết nối" : "máy chủ không phản hồi";
    }
    CloseSocket();
    return false;
}

void DirectAiProtocol::CloseSocket() {
    std::unique_ptr<WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(ws_mutex_);
        ws = std::move(ws_);
    }
    if (ws == nullptr) {
        return;
    }
    muted_ = true;
    const uint32_t conn = conn_id_;
    ws.reset();
    // Stale server messages carry the old id and are dropped.
    conn_id_++;
    Push(InboxItem{kInboxClosed, conn, ""});
}

void DirectAiProtocol::OnSocketClosed(uint32_t conn) {
    if (conn != conn_id_) {
        return;
    }
    xEventGroupSetBits(events_, kFailedBit);
    if (ready_.exchange(false)) {
        ESP_LOGI(TAG, "Server closed the session");
        Push(InboxItem{kInboxClosed, conn, ""});
        NotifyClosed();
    }
}

void DirectAiProtocol::NotifyClosed() {
    Application::GetInstance().PrepareAudioChannelClose();
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void DirectAiProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    const bool was_ready = ready_.exchange(false);
    CloseSocket();
    if (was_ready) {
        NotifyClosed();
    }
}

bool DirectAiProtocol::IsAudioChannelOpened() const {
    return ready_ && !error_occurred_ && !IsTimeout();
}

bool DirectAiProtocol::Send(const std::string& text) {
    if (text.size() > kMaxFrameBytes) {
        ESP_LOGE(TAG, "Message too large (%u bytes)", (unsigned)text.size());
        return false;
    }
    std::lock_guard<std::mutex> lock(ws_mutex_);
    if (ws_ == nullptr || !ws_->IsConnected()) {
        return false;
    }
    return ws_->Send(text);
}

bool DirectAiProtocol::SendText(const std::string& text) {
    return Send(text);
}

void DirectAiProtocol::SendJson(cJSON* root) {
    Send(PrintJson(root));
    cJSON_Delete(root);
}

void DirectAiProtocol::RequestClose() {
    std::weak_ptr<int> alive = alive_;
    Application::GetInstance().Schedule([this, alive]() {
        if (!alive.expired()) {
            CloseAudioChannel();
        }
    });
}

// ---------------------------------------------------------------------------
// Session setup
// ---------------------------------------------------------------------------

std::string DirectAiProtocol::BuildInstructions() const {
    std::string text = kBaseInstructions;
    const std::string& lang = config_.language;
    if (!lang.empty() && lang != "vi" && lang != "auto") {
        text += "\nNgôn ngữ mặc định của người dùng: " + lang + ".";
    }
    if (!config_.instructions.empty()) {
        text += "\n\nYêu cầu thêm của chủ robot:\n" + config_.instructions;
    }
#if CONFIG_BOARD_TYPE_OTTO_ROBOT
    {
        extern std::string OttoSpeakDirectAiPrompt();
        std::string teach = OttoSpeakDirectAiPrompt();
        if (!teach.empty()) {
            text.append("\n\n");
            text.append(teach);
        }
    }
#endif
    return text;
}

std::string DirectAiProtocol::TranscribeLanguage() const {
    return config_.language == "auto" ? "" : config_.language;
}

cJSON* DirectAiProtocol::BuildTools(bool gemini) const {
    cJSON* list = cJSON_CreateArray();
    auto add = [&](const std::string& name, const char* description, cJSON* schema) {
        cJSON* tool = cJSON_CreateObject();
        if (!gemini) {
            cJSON_AddStringToObject(tool, "type", "function");
        }
        cJSON_AddStringToObject(tool, "name", WireToolName(name).c_str());
        cJSON_AddStringToObject(tool, "description", description ? description : "");
        if (gemini) {
            // Robot actions must finish before the model confirms them.
            cJSON_AddStringToObject(tool, "behavior", "BLOCKING");
            // Gemini rejects OBJECT parameters without properties.
            auto props = schema ? cJSON_GetObjectItem(schema, "properties") : nullptr;
            if (cJSON_GetArraySize(props) > 0) {
                cJSON_AddItemToObject(tool, "parameters", GeminiSchema(schema));
            }
            cJSON_Delete(schema);
        } else {
            if (schema == nullptr) {
                schema = cJSON_CreateObject();
                cJSON_AddStringToObject(schema, "type", "object");
                cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
            }
            cJSON_AddItemToObject(tool, "parameters", schema);
        }
        cJSON_AddItemToArray(list, tool);
    };

    for (const auto& json : McpServer::GetInstance().GetAiToolJsons()) {
        cJSON* tool = cJSON_Parse(json.c_str());
        const char* name = JsonString(tool, "name");
        if (name != nullptr) {
            add(name, JsonString(tool, "description"), cJSON_DetachItemFromObject(tool, "inputSchema"));
        }
        cJSON_Delete(tool);
    }
    add(kEndTool, kEndToolDescription, nullptr);
    return list;
}

std::string DirectAiProtocol::BuildOpenAISetup(bool beta) const {
    cJSON* turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "type", "server_vad");
    cJSON_AddNumberToObject(turn, "threshold", 0.5);
    cJSON_AddNumberToObject(turn, "prefix_padding_ms", 300);
    cJSON_AddNumberToObject(turn, "silence_duration_ms", kSilenceMs);
    cJSON_AddBoolToObject(turn, "create_response", true);
    // The mic is closed while the robot speaks; barge-in is the wake word.
    cJSON_AddBoolToObject(turn, "interrupt_response", false);

    cJSON* transcription = cJSON_CreateObject();
    cJSON_AddStringToObject(transcription, "model", "gpt-4o-mini-transcribe");
    std::string lang = TranscribeLanguage();
    if (!lang.empty()) {
        cJSON_AddStringToObject(transcription, "language", lang.c_str());
    }

    cJSON* session = cJSON_CreateObject();
    std::string instructions = BuildInstructions();
    if (beta) {
        cJSON* modalities = cJSON_CreateArray();
        cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
        cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
        cJSON_AddItemToObject(session, "modalities", modalities);
        cJSON_AddStringToObject(session, "instructions", instructions.c_str());
        cJSON_AddStringToObject(session, "voice", config_.voice.c_str());
        cJSON_AddStringToObject(session, "input_audio_format", "pcm16");
        cJSON_AddStringToObject(session, "output_audio_format", "pcm16");
        cJSON_AddItemToObject(session, "input_audio_transcription", transcription);
        cJSON_AddItemToObject(session, "turn_detection", turn);
    } else {
        auto format = []() {
            cJSON* f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "type", "audio/pcm");
            cJSON_AddNumberToObject(f, "rate", kRealtimeRate);
            return f;
        };
        cJSON_AddStringToObject(session, "type", "realtime");
        cJSON_AddStringToObject(session, "instructions", instructions.c_str());
        cJSON* modalities = cJSON_CreateArray();
        cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
        cJSON_AddItemToObject(session, "output_modalities", modalities);
        cJSON* input = cJSON_CreateObject();
        cJSON_AddItemToObject(input, "format", format());
        cJSON_AddItemToObject(input, "transcription", transcription);
        cJSON_AddItemToObject(input, "turn_detection", turn);
        cJSON* noise = cJSON_CreateObject();
        cJSON_AddStringToObject(noise, "type", "far_field");
        cJSON_AddItemToObject(input, "noise_reduction", noise);
        cJSON* output = cJSON_CreateObject();
        cJSON_AddItemToObject(output, "format", format());
        cJSON_AddStringToObject(output, "voice", config_.voice.c_str());
        cJSON* audio = cJSON_CreateObject();
        cJSON_AddItemToObject(audio, "input", input);
        cJSON_AddItemToObject(audio, "output", output);
        cJSON_AddItemToObject(session, "audio", audio);
    }
    cJSON_AddItemToObject(session, "tools", BuildTools(false));
    cJSON_AddStringToObject(session, "tool_choice", "auto");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddItemToObject(root, "session", session);
    std::string out = PrintJson(root);
    cJSON_Delete(root);
    return out;
}

// xAI Voice Agent schema: voice and turn_detection at the top, no reasoning
// for a fast spoken reply.
std::string DirectAiProtocol::BuildGrokSetup(bool search) const {
    auto format = []() {
        cJSON* f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "type", "audio/pcm");
        cJSON_AddNumberToObject(f, "rate", kRealtimeRate);
        return f;
    };
    cJSON* session = cJSON_CreateObject();
    std::string instructions = BuildInstructions();
    cJSON_AddStringToObject(session, "instructions", instructions.c_str());
    cJSON_AddStringToObject(session, "voice", config_.voice.c_str());
    cJSON* reasoning = cJSON_CreateObject();
    cJSON_AddStringToObject(reasoning, "effort", "none");
    cJSON_AddItemToObject(session, "reasoning", reasoning);

    cJSON* turn = cJSON_CreateObject();
    cJSON_AddStringToObject(turn, "type", "server_vad");
    cJSON_AddNumberToObject(turn, "threshold", 0.5);
    cJSON_AddNumberToObject(turn, "prefix_padding_ms", 300);
    cJSON_AddNumberToObject(turn, "silence_duration_ms", kSilenceMs);
    cJSON_AddItemToObject(session, "turn_detection", turn);

    cJSON* input = cJSON_CreateObject();
    cJSON_AddItemToObject(input, "format", format());
    std::string lang = TranscribeLanguage();
    if (!lang.empty()) {
        cJSON* transcription = cJSON_CreateObject();
        cJSON_AddStringToObject(transcription, "language_hint", lang.c_str());
        cJSON_AddItemToObject(input, "transcription", transcription);
    }
    cJSON* output = cJSON_CreateObject();
    cJSON_AddItemToObject(output, "format", format());
    cJSON* audio = cJSON_CreateObject();
    cJSON_AddItemToObject(audio, "input", input);
    cJSON_AddItemToObject(audio, "output", output);
    cJSON_AddItemToObject(session, "audio", audio);

    cJSON* tools = BuildTools(false);
    if (search) {
        cJSON* web = cJSON_CreateObject();
        cJSON_AddStringToObject(web, "type", "web_search");
        cJSON_AddItemToArray(tools, web);
    }
    cJSON_AddItemToObject(session, "tools", tools);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddItemToObject(root, "session", session);
    std::string out = PrintJson(root);
    cJSON_Delete(root);
    return out;
}

std::string DirectAiProtocol::BuildGeminiSetup(bool search) {
    std::string model = config_.model;
    if (model.rfind("models/", 0) != 0) {
        model = "models/" + model;
    }

    cJSON* gen = cJSON_CreateObject();
    cJSON* modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
    cJSON_AddItemToObject(gen, "responseModalities", modalities);
    cJSON* prebuilt = cJSON_CreateObject();
    cJSON_AddStringToObject(prebuilt, "voiceName", config_.voice.c_str());
    cJSON* voice = cJSON_CreateObject();
    cJSON_AddItemToObject(voice, "prebuiltVoiceConfig", prebuilt);
    cJSON* speech = cJSON_CreateObject();
    cJSON_AddItemToObject(speech, "voiceConfig", voice);
    cJSON_AddItemToObject(gen, "speechConfig", speech);
    // 2.5 native-audio models think before every reply; Gemini 3 rejects the setting.
    if (config_.model.find("2.5") != std::string::npos) {
        cJSON* thinking = cJSON_CreateObject();
        cJSON_AddNumberToObject(thinking, "thinkingBudget", 0);
        cJSON_AddItemToObject(gen, "thinkingConfig", thinking);
    }

    cJSON* setup = cJSON_CreateObject();
    cJSON_AddStringToObject(setup, "model", model.c_str());
    cJSON_AddItemToObject(setup, "generationConfig", gen);

    std::string instructions = BuildInstructions();
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", instructions.c_str());
    cJSON* parts = cJSON_CreateArray();
    cJSON_AddItemToArray(parts, part);
    cJSON* system = cJSON_CreateObject();
    cJSON_AddItemToObject(system, "parts", parts);
    cJSON_AddItemToObject(setup, "systemInstruction", system);

    cJSON* tools = cJSON_CreateArray();
    cJSON* functions = cJSON_CreateObject();
    cJSON_AddItemToObject(functions, "functionDeclarations", BuildTools(true));
    cJSON_AddItemToArray(tools, functions);
    if (search) {
        cJSON* google = cJSON_CreateObject();
        cJSON_AddItemToObject(google, "googleSearch", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, google);
    }
    cJSON_AddItemToObject(setup, "tools", tools);

    cJSON* activity = cJSON_CreateObject();
    cJSON_AddNumberToObject(activity, "silenceDurationMs", kSilenceMs);
    cJSON* realtime = cJSON_CreateObject();
    cJSON_AddItemToObject(realtime, "automaticActivityDetection", activity);
    cJSON_AddItemToObject(setup, "realtimeInputConfig", realtime);

    cJSON* input_transcription = cJSON_CreateObject();
    std::string lang = TranscribeLanguage();
    if (!lang.empty()) {
        cJSON* codes = cJSON_CreateArray();
        cJSON_AddItemToArray(codes, cJSON_CreateString(lang.c_str()));
        cJSON_AddItemToObject(input_transcription, "languageCodes", codes);
    }
    cJSON_AddItemToObject(setup, "inputAudioTranscription", input_transcription);
    cJSON_AddItemToObject(setup, "outputAudioTranscription", cJSON_CreateObject());

    // Audio sessions end after ~15 min without compression.
    cJSON* compression = cJSON_CreateObject();
    cJSON_AddItemToObject(compression, "slidingWindow", cJSON_CreateObject());
    cJSON_AddItemToObject(setup, "contextWindowCompression", compression);

    cJSON* resumption = cJSON_CreateObject();
    {
        std::lock_guard<std::mutex> lock(setup_mutex_);
        if (!resume_handle_.empty()) {
            cJSON_AddStringToObject(resumption, "handle", resume_handle_.c_str());
        }
    }
    cJSON_AddItemToObject(setup, "sessionResumption", resumption);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "setup", setup);
    std::string out = PrintJson(root);
    cJSON_Delete(root);
    return out;
}

// ---------------------------------------------------------------------------
// Uplink (main task)
// ---------------------------------------------------------------------------

bool DirectAiProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet->pcm) {
        // Opus packets (wake word audio) mean nothing to the AI servers.
        return true;
    }
    if (!ready_) {
        return false;
    }
    const size_t samples = packet->payload.size() / sizeof(int16_t);
    if (samples == 0) {
        return true;
    }

    std::string message;
    if (IsRealtime(backend_)) {
        if (up_resampler_ == nullptr) {
            return true;
        }
        std::vector<int16_t> pcm(samples);
        memcpy(pcm.data(), packet->payload.data(), samples * sizeof(int16_t));
        std::vector<int16_t> up = Resample(up_resampler_, pcm);
        std::string b64 = Base64Encode(up.data(), up.size() * sizeof(int16_t));
        message.reserve(b64.size() + 64);
        message = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
        message += b64;
        message += "\"}";
        appended_ms_ += static_cast<int>(samples * 1000 / kMicRate);
    } else {
        std::string b64 = Base64Encode(packet->payload.data(), samples * sizeof(int16_t));
        message.reserve(b64.size() + 96);
        message = "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"";
        message += b64;
        message += "\"}}}";
    }
    return Send(message);
}

void DirectAiProtocol::SendUserText(const std::string& text) {
    if (IsRealtime(backend_)) {
        cJSON* content = cJSON_CreateObject();
        cJSON_AddStringToObject(content, "type", "input_text");
        cJSON_AddStringToObject(content, "text", text.c_str());
        cJSON* contents = cJSON_CreateArray();
        cJSON_AddItemToArray(contents, content);
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "message");
        cJSON_AddStringToObject(item, "role", "user");
        cJSON_AddItemToObject(item, "content", contents);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "conversation.item.create");
        cJSON_AddItemToObject(root, "item", item);
        SendJson(root);
        Send("{\"type\":\"response.create\"}");
    } else {
        cJSON* part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", text.c_str());
        cJSON* parts = cJSON_CreateArray();
        cJSON_AddItemToArray(parts, part);
        cJSON* turn = cJSON_CreateObject();
        cJSON_AddStringToObject(turn, "role", "user");
        cJSON_AddItemToObject(turn, "parts", parts);
        cJSON* turns = cJSON_CreateArray();
        cJSON_AddItemToArray(turns, turn);
        cJSON* content = cJSON_CreateObject();
        cJSON_AddItemToObject(content, "turns", turns);
        cJSON_AddBoolToObject(content, "turnComplete", true);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "clientContent", content);
        SendJson(root);
    }
}

void DirectAiProtocol::SendWakeWordDetected(const std::string& wake_word) {
    if (!ready_) {
        return;
    }
    std::string text = wake_word;
    if (text == kSilencePromptText) {
        // First silence: let the AI re-engage. Second one in a row: end the session.
        if (++silence_prompts_ >= 2) {
            ESP_LOGI(TAG, "User stayed silent, closing the session");
            RequestClose();
            return;
        }
        text = kNudgeText;
    }
    last_activity_us_ = NowUs();
    ESP_LOGI(TAG, "User text: %s", text.c_str());
    SendUserText(text);
}

void DirectAiProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    if (ready_ && IsRealtime(backend_)) {
        // Drop mic audio left from before the reply.
        Send("{\"type\":\"input_audio_buffer.clear\"}");
    }
    appended_ms_ = 0;
}

void DirectAiProtocol::SendStopListening() {
    if (!ready_) {
        return;
    }
    if (IsRealtime(backend_)) {
        // Manual (push-to-talk) mode: answer what was said even if server VAD did not fire.
        if (appended_ms_ >= 300) {
            Send("{\"type\":\"input_audio_buffer.commit\"}");
            Send("{\"type\":\"response.create\"}");
        }
        appended_ms_ = 0;
    } else {
        Send("{\"realtimeInput\":{\"audioStreamEnd\":true}}");
    }
}

void DirectAiProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    muted_ = true;
    if (ready_ && IsRealtime(backend_)) {
        Send("{\"type\":\"response.cancel\"}");
    }
    Application::GetInstance().GetAudioService().ResetDecoder();
    Push(InboxItem{kInboxAbort, conn_id_, ""});
    if (speaking_.exchange(false)) {
        EmitTts("stop");
    }
}

void DirectAiProtocol::SendMcpMessage(const std::string& message) {
    Push(InboxItem{kInboxMcpReply, conn_id_, message});
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void DirectAiProtocol::Push(InboxItem&& item) {
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        if (item.kind == kInboxServer && inbox_bytes_ + item.data.size() > kInboxMaxBytes &&
            (item.data.find("\"delta\"") != std::string::npos ||
             item.data.find("inlineData") != std::string::npos)) {
            ESP_LOGW(TAG, "Inbox full (%u bytes), dropping audio", (unsigned)inbox_bytes_);
            return;
        }
        inbox_bytes_ += item.data.size();
        inbox_.push_back(std::move(item));
    }
    inbox_cv_.notify_one();
}

void DirectAiProtocol::WorkerLoop() {
    while (true) {
        InboxItem item;
        bool has_item = false;
        {
            std::unique_lock<std::mutex> lock(inbox_mutex_);
            inbox_cv_.wait_for(lock, std::chrono::milliseconds(200),
                               [this]() { return stop_ || !inbox_.empty(); });
            if (stop_) {
                break;
            }
            if (!inbox_.empty()) {
                item = std::move(inbox_.front());
                inbox_.pop_front();
                inbox_bytes_ -= item.data.size();
                has_item = true;
            }
        }
        if (has_item) {
            switch (item.kind) {
                case kInboxServer:
                    HandleServer(item);
                    break;
                case kInboxMcpReply:
                    HandleMcpReply(item.data);
                    break;
                case kInboxAbort:
                    HandleAbort();
                    break;
                case kInboxClosed:
                    ResetTurn();
                    break;
            }
        }
        Tick();
    }
    xSemaphoreGive(worker_done_);
    vTaskSuspend(nullptr);
}

void DirectAiProtocol::Tick() {
    if (!ready_) {
        return;
    }
    // Never hang up a live lesson/chat because NVS/identity flipped (speak.off,
    // web save, MAC switch). The new config is picked up on the next connect.
    if (config_generation_ != DirectAiConfig::Generation()) {
        config_generation_ = DirectAiConfig::Generation();
        ESP_LOGI(TAG, "Settings changed during session: keep talking, apply on next connect");
        return;
    }
    const int64_t now = NowUs();
    if (Application::GetInstance().GetDeviceState() == kDeviceStateListening &&
        now - last_activity_us_ > kIdleCloseUs) {
        ESP_LOGI(TAG, "Idle, closing the session");
        last_activity_us_ = now;
        RequestClose();
        return;
    }
    if (gemini_turn_complete_ && pending_.empty() && now > gemini_grace_deadline_) {
        FinishTurn();
    }
}

void DirectAiProtocol::HandleServer(const InboxItem& item) {
    if (item.conn != conn_id_) {
        return;
    }
    last_incoming_time_ = std::chrono::steady_clock::now();
    cJSON* root = cJSON_ParseWithLength(item.data.data(), item.data.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Unparsable server message (%u bytes)", (unsigned)item.data.size());
        return;
    }
    if (backend_ == DirectAiBackend::kGemini) {
        HandleGemini(root);
    } else {
        HandleRealtime(root);
    }
    cJSON_Delete(root);
}

void DirectAiProtocol::HandleRealtime(const cJSON* root) {
    const char* type = JsonString(root, "type");
    if (type == nullptr) {
        return;
    }
    const std::string t = type;

    if (t == "session.updated") {
        xEventGroupSetBits(events_, kReadyBit);
    } else if (t == "error") {
        auto err = cJSON_GetObjectItem(root, "error");
        const char* code = JsonString(err, "code");
        const char* message = JsonString(err, "message");
        const std::string c = code ? code : "";
        if (c == "response_cancel_not_active" || c == "conversation_already_has_active_response" ||
            c == "input_audio_buffer_commit_empty") {
            return;
        }
        ESP_LOGW(TAG, "Server error %s: %s", c.c_str(), message ? message : "");
        if (!ready_) {
            std::lock_guard<std::mutex> lock(setup_mutex_);
            setup_error_ = message ? message : c;
            xEventGroupSetBits(events_, kFailedBit);
        } else if (!speaking_ && message != nullptr) {
            ShowError(message);
        }
    } else if (t == "input_audio_buffer.speech_started") {
        OnUserSpeech();
    } else if (t == "input_audio_buffer.committed") {
        appended_ms_ = 0;
    } else if (t == "conversation.item.input_audio_transcription.completed") {
        const char* transcript = JsonString(root, "transcript");
        if (transcript != nullptr && transcript[0] != '\0') {
            EmitStt(transcript);
        }
        OnUserSpeech();
    } else if (t == "response.created") {
        response_active_ = true;
    } else if (t == "response.output_audio.delta" || t == "response.audio.delta") {
        // xAI may name the field "audio".
        const char* delta = JsonString(root, "delta");
        if (delta == nullptr) {
            delta = JsonString(root, "audio");
        }
        if (delta != nullptr) {
            PlayAudio(delta, strlen(delta), kRealtimeRate);
        }
    } else if (t == "response.output_audio_transcript.delta" || t == "response.audio_transcript.delta") {
        const char* delta = JsonString(root, "delta");
        if (delta != nullptr) {
            reply_text_ += delta;
        }
    } else if (t == "response.output_audio_transcript.done" || t == "response.audio_transcript.done") {
        const char* transcript = JsonString(root, "transcript");
        if (transcript != nullptr && transcript[0] != '\0') {
            reply_text_ = transcript;
        }
        if (!reply_text_.empty()) {
            EmitAssistant(reply_text_);
            reply_text_.clear();
        }
    } else if (t == "response.function_call_arguments.done") {
        const char* call_id = JsonString(root, "call_id");
        const char* name = JsonString(root, "name");
        const char* arguments = JsonString(root, "arguments");
        if (call_id == nullptr || name == nullptr) {
            return;
        }
        cJSON* args = arguments && arguments[0] ? cJSON_Parse(arguments) : nullptr;
        RunTool(call_id, name, args);
        cJSON_Delete(args);
    } else if (t == "response.done") {
        response_active_ = false;
        auto response = cJSON_GetObjectItem(root, "response");
        const char* status = JsonString(response, "status");
        if (status != nullptr && strcmp(status, "failed") == 0) {
            auto details = cJSON_GetObjectItem(response, "status_details");
            auto error = cJSON_GetObjectItem(details, "error");
            const char* message = JsonString(error, "message");
            std::string text = message ? message : PrintJson(details);
            ESP_LOGW(TAG, "Response failed: %s", text.c_str());
            if (!speaking_) {
                ShowError(text);
            }
        }
        if (!reply_text_.empty()) {
            EmitAssistant(reply_text_);
            reply_text_.clear();
        }
        TryFollowUp();
        MaybeFinishRealtimeTurn();
    }
}

void DirectAiProtocol::HandleGemini(const cJSON* root) {
    if (cJSON_GetObjectItem(root, "setupComplete") != nullptr) {
        xEventGroupSetBits(events_, kReadyBit);
        return;
    }
    auto resumption = cJSON_GetObjectItem(root, "sessionResumptionUpdate");
    if (resumption != nullptr) {
        const char* handle = JsonString(resumption, "newHandle");
        if (cJSON_IsTrue(cJSON_GetObjectItem(resumption, "resumable")) && handle != nullptr && handle[0]) {
            std::lock_guard<std::mutex> lock(setup_mutex_);
            resume_handle_ = handle;
        }
        return;
    }
    if (cJSON_GetObjectItem(root, "goAway") != nullptr) {
        ESP_LOGI(TAG, "Gemini announced the end of this connection");
        return;
    }

    auto tool_call = cJSON_GetObjectItem(root, "toolCall");
    if (tool_call != nullptr) {
        gemini_in_turn_ = true;
        gemini_turn_complete_ = false;
        const cJSON* call = nullptr;
        cJSON_ArrayForEach(call, cJSON_GetObjectItem(tool_call, "functionCalls")) {
            const char* id = JsonString(call, "id");
            const char* name = JsonString(call, "name");
            if (name == nullptr) {
                continue;
            }
            cJSON* args = cJSON_Duplicate(cJSON_GetObjectItem(call, "args"), true);
            RunTool(id ? id : "", name, args);
            cJSON_Delete(args);
        }
        if (pending_.empty() && !gemini_results_.empty()) {
            SendGeminiToolResponse();
        }
        return;
    }

    auto content = cJSON_GetObjectItem(root, "serverContent");
    if (content == nullptr) {
        return;
    }
    auto input = cJSON_GetObjectItem(content, "inputTranscription");
    const char* heard = JsonString(input, "text");
    if (heard != nullptr) {
        user_text_ += heard;
        OnUserSpeech();
    }
    auto output = cJSON_GetObjectItem(content, "outputTranscription");
    const char* said = JsonString(output, "text");
    if (said != nullptr) {
        reply_text_ += said;
        gemini_in_turn_ = true;
        gemini_tool_followup_ = false;
        gemini_turn_complete_ = false;
    }
    auto model_turn = cJSON_GetObjectItem(content, "modelTurn");
    const cJSON* part = nullptr;
    cJSON_ArrayForEach(part, cJSON_GetObjectItem(model_turn, "parts")) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(part, "thought"))) {
            continue;
        }
        auto inline_data = cJSON_GetObjectItem(part, "inlineData");
        const char* mime = JsonString(inline_data, "mimeType");
        const char* data = JsonString(inline_data, "data");
        if (mime == nullptr || data == nullptr || strncmp(mime, "audio/pcm", 9) != 0) {
            continue;
        }
        gemini_in_turn_ = true;
        gemini_tool_followup_ = false;
        gemini_turn_complete_ = false;
        FlushStt();
        PlayAudio(data, strlen(data), RateFromMime(mime, kRealtimeRate));
    }
    // Server VAD heard the user over the reply; that turn gets no turnComplete.
    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "interrupted"))) {
        FinishTurn();
        return;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "turnComplete"))) {
        if (!pending_.empty() || gemini_tool_followup_) {
            gemini_turn_complete_ = true;
            gemini_grace_deadline_ = NowUs() + kToolGraceUs;
        } else {
            FinishTurn();
        }
    }
}

void DirectAiProtocol::RunTool(const std::string& call_id, const std::string& wire_name, cJSON* args) {
    const std::string name = McpToolName(wire_name);
    std::string args_text = args ? PrintJson(args) : "{}";
    ESP_LOGI(TAG, "Tool call %s %s", name.c_str(), args_text.c_str());

    if (name == kEndTool) {
        end_requested_ = true;
        const std::string ok = "{\"ok\":true}";
        if (backend_ == DirectAiBackend::kGemini) {
            gemini_results_.push_back(ToolResult{call_id, wire_name, ok});
        } else {
            SendRealtimeToolOutput(call_id, ok);
            // Goodbye is usually spoken before the call; only ask for it if not.
            need_followup_ = !speaking_;
        }
        return;
    }

    const int id = next_rpc_id_++;
    pending_[id] = PendingCall{call_id, wire_name};

    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", name.c_str());
    cJSON_AddItemToObject(params, "arguments",
                          cJSON_IsObject(args) ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
    cJSON* rpc = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rpc, "id", id);
    cJSON_AddStringToObject(rpc, "method", "tools/call");
    cJSON_AddItemToObject(rpc, "params", params);
    // The reply arrives through SendMcpMessage().
    McpServer::GetInstance().ParseMessage(rpc);
    cJSON_Delete(rpc);
}

void DirectAiProtocol::HandleMcpReply(const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        return;
    }
    auto id_item = cJSON_GetObjectItem(root, "id");
    auto it = cJSON_IsNumber(id_item) ? pending_.find(id_item->valueint) : pending_.end();
    if (it == pending_.end()) {
        cJSON_Delete(root);
        return;
    }
    PendingCall call = it->second;
    pending_.erase(it);

    std::string output;
    auto result = cJSON_GetObjectItem(root, "result");
    if (result != nullptr) {
        auto first = cJSON_GetArrayItem(cJSON_GetObjectItem(result, "content"), 0);
        const char* text = JsonString(first, "text");
        output = text ? text : "{\"ok\":true}";
        if (cJSON_IsTrue(cJSON_GetObjectItem(result, "isError"))) {
            output = "error: " + output;
        }
    } else {
        const char* message = JsonString(cJSON_GetObjectItem(root, "error"), "message");
        output = std::string("error: ") + (message ? message : "unknown");
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Tool result %s: %.120s", McpToolName(call.wire_name).c_str(), output.c_str());

    if (backend_ == DirectAiBackend::kGemini) {
        gemini_results_.push_back(ToolResult{call.call_id, call.wire_name, output});
        if (pending_.empty()) {
            SendGeminiToolResponse();
        }
    } else {
        SendRealtimeToolOutput(call.call_id, output);
        TryFollowUp();
        MaybeFinishRealtimeTurn();
    }
}

void DirectAiProtocol::SendRealtimeToolOutput(const std::string& call_id, const std::string& output) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id.c_str());
    cJSON_AddStringToObject(item, "output", output.c_str());
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "conversation.item.create");
    cJSON_AddItemToObject(root, "item", item);
    SendJson(root);
    need_followup_ = true;
}

void DirectAiProtocol::SendGeminiToolResponse() {
    cJSON* responses = cJSON_CreateArray();
    for (const auto& r : gemini_results_) {
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "result", r.output.c_str());
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", r.call_id.c_str());
        cJSON_AddStringToObject(item, "name", r.wire_name.c_str());
        cJSON_AddItemToObject(item, "response", response);
        cJSON_AddItemToArray(responses, item);
    }
    gemini_results_.clear();
    cJSON* tool_response = cJSON_CreateObject();
    cJSON_AddItemToObject(tool_response, "functionResponses", responses);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "toolResponse", tool_response);
    SendJson(root);
    gemini_tool_followup_ = true;
    gemini_grace_deadline_ = NowUs() + kToolGraceUs;
}

void DirectAiProtocol::TryFollowUp() {
    if (abort_turn_) {
        need_followup_ = false;
        return;
    }
    if (need_followup_ && pending_.empty() && !response_active_) {
        need_followup_ = false;
        response_active_ = true;
        Send("{\"type\":\"response.create\"}");
    }
}

void DirectAiProtocol::MaybeFinishRealtimeTurn() {
    if (!response_active_ && pending_.empty() && !need_followup_) {
        FinishTurn();
    }
}

void DirectAiProtocol::HandleAbort() {
    abort_turn_ = true;
    need_followup_ = false;
    if (backend_ == DirectAiBackend::kGemini) {
        if (!gemini_in_turn_) {
            FinishTurn();
        }
    } else {
        MaybeFinishRealtimeTurn();
    }
}

void DirectAiProtocol::ResetTurn() {
    pending_.clear();
    gemini_results_.clear();
    response_active_ = false;
    need_followup_ = false;
    abort_turn_ = false;
    end_requested_ = false;
    gemini_in_turn_ = false;
    gemini_turn_complete_ = false;
    gemini_tool_followup_ = false;
    user_text_.clear();
    reply_text_.clear();
    speaking_ = false;
    gate_ = kGateWaiting;
    muted_ = false;
}

void DirectAiProtocol::FinishTurn() {
    FlushTranscripts();
    const bool end = end_requested_;
    pending_.clear();
    gemini_results_.clear();
    response_active_ = false;
    need_followup_ = false;
    abort_turn_ = false;
    end_requested_ = false;
    gemini_in_turn_ = false;
    gemini_turn_complete_ = false;
    gemini_tool_followup_ = false;
    muted_ = false;

    if (end) {
        // Let the goodbye play out, then close like the Xiaozhi server does.
        if (speaking_) {
            Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
        }
        speaking_ = false;
        gate_ = kGateWaiting;
        ESP_LOGI(TAG, "Conversation ended by the user");
        RequestClose();
        return;
    }
    if (speaking_.exchange(false)) {
        EmitTts("stop");
    }
    gate_ = kGateWaiting;
}

void DirectAiProtocol::PlayAudio(const char* b64, size_t len, int rate) {
    if (muted_) {
        return;
    }
    std::vector<uint8_t> raw;
    if (!Base64Decode(b64, len, raw) || raw.size() < sizeof(int16_t)) {
        return;
    }
    std::vector<int16_t> pcm(raw.size() / sizeof(int16_t));
    memcpy(pcm.data(), raw.data(), pcm.size() * sizeof(int16_t));
    raw = std::vector<uint8_t>();

    if (rate != codec_rate_) {
        if (down_resampler_ == nullptr || down_rate_ != rate) {
            if (down_resampler_ != nullptr) {
                esp_ae_rate_cvt_close(down_resampler_);
            }
            down_resampler_ = OpenResampler(rate, codec_rate_);
            down_rate_ = rate;
        }
        if (down_resampler_ == nullptr) {
            return;
        }
        pcm = Resample(down_resampler_, pcm);
    }

    if (!speaking_) {
        BeginSpeaking();
    }
    if (!WaitGate()) {
        return;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    const size_t chunk = static_cast<size_t>(codec_rate_) * kPlaybackChunkMs / 1000;
    for (size_t offset = 0; offset < pcm.size(); offset += chunk) {
        if (muted_) {
            return;
        }
        const size_t n = std::min(chunk, pcm.size() - offset);
        audio.EnqueuePlaybackPcm(std::vector<int16_t>(pcm.begin() + offset, pcm.begin() + offset + n));
    }
    last_activity_us_ = NowUs();
}

void DirectAiProtocol::BeginSpeaking() {
    speaking_ = true;
    gate_ = kGateWaiting;
    EmitTts("start");
    const uint32_t seq = ++speak_seq_;
    std::weak_ptr<int> alive = alive_;
    Application::GetInstance().Schedule([this, alive, seq]() {
        // Second hop: runs after Application handled the Speaking state change
        // (which clears the playback queue), so queued speech is not dropped.
        Application::GetInstance().Schedule([this, alive, seq]() {
            if (alive.expired() || seq != speak_seq_) {
                return;
            }
            const bool speaking = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
            gate_ = speaking ? kGateOpen : kGateFailed;
        });
    });
}

bool DirectAiProtocol::WaitGate() {
    const int64_t start = NowUs();
    while (gate_ == kGateWaiting) {
        if (muted_) {
            return false;
        }
        if (NowUs() - start > kGateWaitUs) {
            ESP_LOGW(TAG, "Device did not enter the speaking state, dropping audio");
            gate_ = kGateFailed;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return gate_ == kGateOpen && !muted_;
}

void DirectAiProtocol::OnUserSpeech() {
    last_activity_us_ = NowUs();
    silence_prompts_ = 0;
}

void DirectAiProtocol::ShowError(const std::string& message) {
    Application::GetInstance().Schedule([message]() {
        Board::GetInstance().GetDisplay()->SetChatMessage("system", ("AI: " + message).c_str());
    });
}

// ---------------------------------------------------------------------------
// Events for Application
// ---------------------------------------------------------------------------

void DirectAiProtocol::Emit(cJSON* root) {
    if (on_incoming_json_ != nullptr) {
        on_incoming_json_(root);
    }
    cJSON_Delete(root);
}

void DirectAiProtocol::EmitTts(const char* state) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    Emit(root);
}

void DirectAiProtocol::EmitStt(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "stt");
    cJSON_AddStringToObject(root, "text", text.c_str());
    Emit(root);
}

void DirectAiProtocol::EmitAssistant(const std::string& text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", "sentence_start");
    cJSON_AddStringToObject(root, "text", text.c_str());
    Emit(root);
}

void DirectAiProtocol::FlushStt() {
    if (!user_text_.empty()) {
        EmitStt(user_text_);
        user_text_.clear();
    }
}

void DirectAiProtocol::FlushTranscripts() {
    FlushStt();
    if (!reply_text_.empty()) {
        EmitAssistant(reply_text_);
        reply_text_.clear();
    }
}

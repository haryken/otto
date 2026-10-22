#include "direct_ai_config.h"
#include "device_identity_presets.h"
#include "settings.h"
#include "sdkconfig.h"
#if CONFIG_BOARD_TYPE_OTTO_ROBOT
#include "speak_lesson.h"
#endif

#include <atomic>
#include <cctype>
#include <cstring>
#include <esp_log.h>
#include <nvs.h>

#define TAG "DirectAiConfig"

namespace {

constexpr const char* kNamespace = "direct_ai";
constexpr size_t kMaxInstructions = 2000;  // UTF-8 bytes (~600 Vietnamese characters)
constexpr size_t kMaxKey = 300;
constexpr size_t kMaxName = 80;

struct BackendInfo {
    DirectAiBackend backend;
    const char* name;
    const char* prefix;  // NVS key prefix
    const char* default_model;
    const char* default_voice;
};

constexpr BackendInfo kBackends[] = {
    {DirectAiBackend::kOpenAI, "openai", "oa", "gpt-realtime-mini", "coral"},
    {DirectAiBackend::kGrok, "grok", "gk", "grok-voice-latest", "eve"},
    {DirectAiBackend::kGemini, "gemini", "gm", "gemini-3.8-live", "Kore"},
};

std::atomic<DirectAiBackend> g_running{DirectAiBackend::kOff};
std::atomic<uint32_t> g_generation{0};

const BackendInfo* Info(DirectAiBackend backend) {
    for (const auto& info : kBackends) {
        if (info.backend == backend) {
            return &info;
        }
    }
    return nullptr;
}

std::string NvsKey(const BackendInfo& info, const char* field) {
    return std::string(info.prefix) + "_" + field;
}

std::string MaskKey(const std::string& key) {
    if (key.empty()) {
        return "";
    }
    if (key.size() <= 10) {
        return "…" + key.substr(key.size() - 2);
    }
    return key.substr(0, 3) + "…" + key.substr(key.size() - 4);
}

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) {
        b++;
    }
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) {
        e--;
    }
    return s.substr(b, e - b);
}

// Settings::SetString aborts on any NVS error; the NVS partition is small, so
// web saves go through this writer and report "full" instead of rebooting.
class NvsWriter {
public:
    NvsWriter() {
        err_ = nvs_open(kNamespace, NVS_READWRITE, &handle_);
    }
    ~NvsWriter() {
        if (handle_ != 0) {
            nvs_commit(handle_);
            nvs_close(handle_);
        }
    }
    // Empty string erases the key so the default applies again.
    void PutString(const std::string& key, const std::string& value) {
        if (err_ != ESP_OK) {
            return;
        }
        if (value.empty()) {
            esp_err_t ret = nvs_erase_key(handle_, key.c_str());
            if (ret != ESP_ERR_NVS_NOT_FOUND) {
                Check(ret);
            }
        } else {
            Check(nvs_set_str(handle_, key.c_str(), value.c_str()));
        }
    }
    void PutBool(const std::string& key, bool value) {
        if (err_ == ESP_OK) {
            Check(nvs_set_u8(handle_, key.c_str(), value ? 1 : 0));
        }
    }
    void Erase(const std::string& key) {
        PutString(key, "");
    }
    esp_err_t error() const { return err_; }

private:
    nvs_handle_t handle_ = 0;
    esp_err_t err_ = ESP_OK;

    void Check(esp_err_t ret) {
        if (ret != ESP_OK && err_ == ESP_OK) {
            err_ = ret;
        }
    }
};

}  // namespace

const char* DirectAiConfig::BackendName(DirectAiBackend backend) {
    auto info = Info(backend);
    return info ? info->name : "xiaozhi";
}

DirectAiBackend DirectAiConfig::ParseBackend(const std::string& name) {
    for (const auto& info : kBackends) {
        if (name == info.name) {
            return info.backend;
        }
    }
    return DirectAiBackend::kOff;
}

DirectAiBackend DirectAiConfig::LoadBackend() {
#if CONFIG_BOARD_TYPE_OTTO_ROBOT
    if (SpeakLesson::Get().UsesNewIdentity()) {
        return ParseBackend(SpeakLesson::Get().ActiveAiBackendName());
    }
#endif
    Settings settings(kNamespace, false);
    return ParseBackend(settings.GetString("backend"));
}

DirectAiConfig DirectAiConfig::Load() {
    Settings settings(kNamespace, false);
    DirectAiConfig cfg;
    cfg.language = settings.GetString("lang", "vi");
    cfg.search = settings.GetBool("search", true);
#if CONFIG_BOARD_TYPE_OTTO_ROBOT
    if (SpeakLesson::Get().UsesNewIdentity()) {
        auto& lesson = SpeakLesson::Get();
        DirectAiBackend overlay = ParseBackend(lesson.ActiveAiBackendName());
        if (overlay != DirectAiBackend::kOff) {
            cfg.backend = overlay;
            cfg.instructions = lesson.ActiveAiInstructions();
            // Local teaching cards; googleSearch often 1011-quota-kills the first session.
            cfg.search = false;
            if (auto info = Info(cfg.backend)) {
                cfg.key = lesson.ActiveAiKey();
                std::string model = lesson.ActiveAiModel();
                std::string voice = lesson.ActiveAiVoice();
                cfg.model = model.empty() ? info->default_model : model;
                cfg.voice = voice.empty() ? info->default_voice : voice;
            }
            return cfg;
        }
        // New-style Xiaozhi (teach / daily / custom) uses MQTT/WS, never World Edu Direct AI.
        cfg.backend = DirectAiBackend::kOff;
        return cfg;
    }
#endif
    cfg.backend = ParseBackend(settings.GetString("backend"));
    cfg.instructions = settings.GetString("instr");
    if (ReadPresetMacIndexFromNvs() == kDailyChatPresetMacIndex) {
        std::string daily = settings.GetString("instr_d");
        if (!daily.empty()) {
            cfg.instructions = std::move(daily);
        }
    }
    if (auto info = Info(cfg.backend)) {
        cfg.key = settings.GetString(NvsKey(*info, "key"));
        cfg.model = settings.GetString(NvsKey(*info, "model"), info->default_model);
        cfg.voice = settings.GetString(NvsKey(*info, "voice"), info->default_voice);
    }
    return cfg;
}

void DirectAiConfig::SetRunningBackend(DirectAiBackend backend) {
    g_running.store(backend);
}

DirectAiBackend DirectAiConfig::RunningBackend() {
    return g_running.load();
}

uint32_t DirectAiConfig::Generation() {
    return g_generation.load();
}

void DirectAiConfig::BumpGeneration() {
    g_generation.fetch_add(1);
}

std::string DirectAiConfig::ToWebJson() {
    Settings settings(kNamespace, false);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "backend",
        BackendName(ParseBackend(settings.GetString("backend"))));
    cJSON_AddStringToObject(root, "running", BackendName(RunningBackend()));
    cJSON_AddStringToObject(root, "language", settings.GetString("lang", "vi").c_str());
    cJSON_AddStringToObject(root, "instructions", settings.GetString("instr").c_str());
    cJSON_AddStringToObject(root, "instructions_daily", settings.GetString("instr_d").c_str());
    cJSON_AddBoolToObject(root, "search", settings.GetBool("search", true));
    for (const auto& info : kBackends) {
        cJSON* item = cJSON_CreateObject();
        std::string key = settings.GetString(NvsKey(info, "key"));
        cJSON_AddBoolToObject(item, "has_key", !key.empty());
        cJSON_AddStringToObject(item, "key_hint", MaskKey(key).c_str());
        cJSON_AddStringToObject(item, "model", settings.GetString(NvsKey(info, "model")).c_str());
        cJSON_AddStringToObject(item, "voice", settings.GetString(NvsKey(info, "voice")).c_str());
        cJSON_AddStringToObject(item, "default_model", info.default_model);
        cJSON_AddStringToObject(item, "default_voice", info.default_voice);
        cJSON_AddItemToObject(root, info.name, item);
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return out;
}

bool DirectAiConfig::ApplyWebJson(const cJSON* root, bool* backend_changed, std::string* error) {
    *backend_changed = false;
    if (!cJSON_IsObject(root)) {
        *error = "Invalid JSON";
        return false;
    }

    auto backend = cJSON_GetObjectItem(root, "backend");
    if (backend != nullptr && !cJSON_IsString(backend)) {
        *error = "backend must be a string";
        return false;
    }
    auto instructions = cJSON_GetObjectItem(root, "instructions");
    if (cJSON_IsString(instructions) && strlen(instructions->valuestring) > kMaxInstructions) {
        *error = "Lời dặn World Edu quá dài (tối đa khoảng 600 ký tự)";
        return false;
    }
    auto instructions_daily = cJSON_GetObjectItem(root, "instructions_daily");
    if (cJSON_IsString(instructions_daily) && strlen(instructions_daily->valuestring) > kMaxInstructions) {
        *error = "Lời dặn giao tiếp hằng ngày quá dài (tối đa khoảng 600 ký tự)";
        return false;
    }

    auto current = LoadBackend();
    auto next = cJSON_IsString(backend) ? ParseBackend(backend->valuestring) : current;

    {
        NvsWriter nvs;
        if (next != current) {
            nvs.PutString("backend", next == DirectAiBackend::kOff ? "" : BackendName(next));
        }
        auto language = cJSON_GetObjectItem(root, "language");
        if (cJSON_IsString(language)) {
            nvs.PutString("lang", Trim(language->valuestring).substr(0, 16));
        }
        if (cJSON_IsString(instructions)) {
            nvs.PutString("instr", Trim(instructions->valuestring));
        }
        if (cJSON_IsString(instructions_daily)) {
            nvs.PutString("instr_d", Trim(instructions_daily->valuestring));
        }
        auto search = cJSON_GetObjectItem(root, "search");
        if (cJSON_IsBool(search)) {
            nvs.PutBool("search", cJSON_IsTrue(search));
        }

        for (const auto& info : kBackends) {
            auto item = cJSON_GetObjectItem(root, info.name);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            auto key = cJSON_GetObjectItem(item, "key");
            if (cJSON_IsString(key)) {
                std::string value = Trim(key->valuestring);
                if (!value.empty()) {
                    nvs.PutString(NvsKey(info, "key"), value.substr(0, kMaxKey));
                }
            }
            if (cJSON_IsTrue(cJSON_GetObjectItem(item, "clear_key"))) {
                nvs.Erase(NvsKey(info, "key"));
            }
            auto model = cJSON_GetObjectItem(item, "model");
            if (cJSON_IsString(model)) {
                nvs.PutString(NvsKey(info, "model"), Trim(model->valuestring).substr(0, kMaxName));
            }
            auto voice = cJSON_GetObjectItem(item, "voice");
            if (cJSON_IsString(voice)) {
                nvs.PutString(NvsKey(info, "voice"), Trim(voice->valuestring).substr(0, kMaxName));
            }
        }

        if (nvs.error() != ESP_OK) {
            ESP_LOGE(TAG, "Save failed: %s", esp_err_to_name(nvs.error()));
            *error = nvs.error() == ESP_ERR_NVS_NOT_ENOUGH_SPACE
                ? "Bộ nhớ cài đặt (NVS) đã đầy"
                : std::string("Lỗi lưu NVS: ") + esp_err_to_name(nvs.error());
            return false;
        }
    }

    *backend_changed = next != current;
    g_generation.fetch_add(1);
    ESP_LOGI(TAG, "Saved (backend %s%s)", BackendName(next), *backend_changed ? ", changed" : "");
    return true;
}

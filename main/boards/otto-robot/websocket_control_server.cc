#include "websocket_control_server.h"
#include "mcp_server.h"
#include "application.h"
#include "board.h"
#include "audio_codec.h"
#include "device_identity_presets.h"
#include "system_info.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <nvs.h>
#include <sys/param.h>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <map>
#include <string>
#include <algorithm>

static const char* TAG = "WSControl";

#define OTTO_NVS_NS "otto"
#define WIFI_NVS_NS "wifi"
#define NVS_KEY_STUDENT_NAME "student_name"
#define NVS_KEY_PRESET_MAC "preset_mac"
#define NVS_KEY_UNITS_PREFIX "units_"
#define NVS_KEY_YI_SUB "yi_sub"
#define NVS_KEY_YI_UNIT_PREFIX "yi_u"
#define NVS_KEY_EX_SUB "ex_sub"
#define NVS_KEY_EX_UNIT_PREFIX "ex_u"
#define NVS_KEY_FL_SUB "fl_sub"
#define NVS_KEY_FL_UNIT_PREFIX "fl_u"
#define DEFAULT_UNITS "1"
#define EXPLORERS_IDX 1
#define YOUNG_INNOVATORS_IDX 2
#define FUTURE_LEADERS_IDX 3
#define EXPLORERS_SUB_COUNT 5
#define YI_SUB_COUNT 3
#define FL_SUB_COUNT 1

extern const char self_control_html_start[] asm("_binary_self_control_html_start");
extern const char self_control_html_end[] asm("_binary_self_control_html_end");

WebSocketControlServer* WebSocketControlServer::instance_ = nullptr;

WebSocketControlServer::WebSocketControlServer() : server_handle_(nullptr) {
    instance_ = this;
}

WebSocketControlServer::~WebSocketControlServer() {
    Stop();
    instance_ = nullptr;
}

esp_err_t WebSocketControlServer::ws_handler(httpd_req_t *req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        instance_->AddClient(req);
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }
    
    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);
    
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket close frame received");
        instance_->RemoveClient(req);
        free(buf);
        return ESP_OK;
    }
    
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        if (ws_pkt.len > 0 && buf != nullptr) {
            buf[ws_pkt.len] = '\0';
            instance_->HandleMessage(req, (const char*)buf, ws_pkt.len);
        }
    } else {
        ESP_LOGW(TAG, "Unsupported frame type: %d", ws_pkt.type);
    }
    
    free(buf);
    return ESP_OK;
}

// ========== NVS Helpers ==========

std::string WebSocketControlServer::GetStudentName() {
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return "";
    size_t len = 0;
    if (nvs_get_str(nvs, NVS_KEY_STUDENT_NAME, nullptr, &len) != ESP_OK || len == 0) {
        nvs_close(nvs);
        return "";
    }
    char* buf = (char*)malloc(len);
    nvs_get_str(nvs, NVS_KEY_STUDENT_NAME, buf, &len);
    std::string name(buf);
    free(buf);
    nvs_close(nvs);
    return name;
}

int WebSocketControlServer::GetPresetMacIdx() {
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return 1;
    int32_t idx = 1;
    nvs_get_i32(nvs, NVS_KEY_PRESET_MAC, &idx);
    nvs_close(nvs);
    if (idx < 0 || idx > 7) return 1;
    return (int)idx;
}

static bool IsValidMacString(const char* mac) {
    if (mac == nullptr || mac[0] == '\0') return false;
    unsigned int b[6];
    char extra = 0;
    if (sscanf(mac, "%02x:%02x:%02x:%02x:%02x:%02x%c",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &extra) != 6) {
        return false;
    }
    return true;
}

static std::string NormalizeMacString(const char* mac) {
    if (mac == nullptr) return "";
    std::string out;
    out.reserve(17);
    for (const char* p = mac; *p; ++p) {
        char c = *p;
        if (c == '-' || c == ' ') c = ':';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

static std::string GetCustomMacFromNvs() {
    char buf[CUSTOM_MAC_STR_MAX] = {0};
    if (!ReadCustomMacFromNvs(buf, sizeof(buf))) return "";
    return std::string(buf);
}

static void SaveCustomMacToWifiNvs(const char* mac) {
    WriteCustomMacToNvs(mac ? mac : "");
}

int WebSocketControlServer::GetYoungInnovatorsSubIdx() {
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return 0;
    int32_t idx = 0;
    nvs_get_i32(nvs, NVS_KEY_YI_SUB, &idx);
    nvs_close(nvs);
    if (idx < 0 || idx >= YI_SUB_COUNT) return 0;
    return (int)idx;
}

std::string WebSocketControlServer::GetYoungInnovatorsUnit(int sub_idx) {
    if (sub_idx < 0 || sub_idx >= YI_SUB_COUNT) sub_idx = 0;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return DEFAULT_UNITS;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_YI_UNIT_PREFIX, sub_idx);
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) {
        nvs_close(nvs);
        return DEFAULT_UNITS;
    }
    char* buf = (char*)malloc(len);
    nvs_get_str(nvs, key, buf, &len);
    std::string units(buf);
    free(buf);
    nvs_close(nvs);
    return units.empty() ? DEFAULT_UNITS : units;
}

static void SaveYoungInnovatorsSubIdx(int idx) {
    if (idx < 0 || idx >= YI_SUB_COUNT) return;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_i32(nvs, NVS_KEY_YI_SUB, (int32_t)idx);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void SaveYoungInnovatorsUnit(int sub_idx, const char* units) {
    if (sub_idx < 0 || sub_idx >= YI_SUB_COUNT || units == nullptr) return;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_YI_UNIT_PREFIX, sub_idx);
    nvs_set_str(nvs, key, units);
    nvs_commit(nvs);
    nvs_close(nvs);
}

int WebSocketControlServer::GetExplorersSubIdx() {
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return 0;
    int32_t idx = 0;
    nvs_get_i32(nvs, NVS_KEY_EX_SUB, &idx);
    nvs_close(nvs);
    if (idx < 0 || idx >= EXPLORERS_SUB_COUNT) return 0;
    return (int)idx;
}

std::string WebSocketControlServer::GetExplorersUnit(int sub_idx) {
    if (sub_idx < 0 || sub_idx >= EXPLORERS_SUB_COUNT) sub_idx = 0;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return DEFAULT_UNITS;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_EX_UNIT_PREFIX, sub_idx);
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) {
        nvs_close(nvs);
        return DEFAULT_UNITS;
    }
    char* buf = (char*)malloc(len);
    nvs_get_str(nvs, key, buf, &len);
    std::string units(buf);
    free(buf);
    nvs_close(nvs);
    return units.empty() ? DEFAULT_UNITS : units;
}

static void SaveExplorersSubIdx(int idx) {
    if (idx < 0 || idx >= EXPLORERS_SUB_COUNT) return;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_i32(nvs, NVS_KEY_EX_SUB, (int32_t)idx);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void SaveExplorersUnit(int sub_idx, const char* units) {
    if (sub_idx < 0 || sub_idx >= EXPLORERS_SUB_COUNT || units == nullptr) return;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_EX_UNIT_PREFIX, sub_idx);
    nvs_set_str(nvs, key, units);
    nvs_commit(nvs);
    nvs_close(nvs);
}

int WebSocketControlServer::GetFutureLeadersSubIdx() {
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return 0;
    int32_t idx = 0;
    nvs_get_i32(nvs, NVS_KEY_FL_SUB, &idx);
    nvs_close(nvs);
    if (idx < 0 || idx >= FL_SUB_COUNT) return 0;
    return (int)idx;
}

std::string WebSocketControlServer::GetFutureLeadersUnit(int sub_idx) {
    if (sub_idx < 0 || sub_idx >= FL_SUB_COUNT) sub_idx = 0;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return DEFAULT_UNITS;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_FL_UNIT_PREFIX, sub_idx);
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) {
        nvs_close(nvs);
        return DEFAULT_UNITS;
    }
    char* buf = (char*)malloc(len);
    nvs_get_str(nvs, key, buf, &len);
    std::string units(buf);
    free(buf);
    nvs_close(nvs);
    return units.empty() ? DEFAULT_UNITS : units;
}

static void SaveFutureLeadersSubIdx(int idx) {
    if (idx < 0 || idx >= FL_SUB_COUNT) return;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_i32(nvs, NVS_KEY_FL_SUB, (int32_t)idx);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void SaveFutureLeadersUnit(int sub_idx, const char* units) {
    if (sub_idx < 0 || sub_idx >= FL_SUB_COUNT || units == nullptr) return;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_FL_UNIT_PREFIX, sub_idx);
    nvs_set_str(nvs, key, units);
    nvs_commit(nvs);
    nvs_close(nvs);
}

int WebSocketControlServer::GetActiveSubIdx(int course_idx) {
    if (course_idx == EXPLORERS_IDX) return GetExplorersSubIdx();
    if (course_idx == YOUNG_INNOVATORS_IDX) return GetYoungInnovatorsSubIdx();
    if (course_idx == FUTURE_LEADERS_IDX) return GetFutureLeadersSubIdx();
    return 0;
}

std::string WebSocketControlServer::GetActiveUnitSelection(int course_idx) {
    if (course_idx == EXPLORERS_IDX) {
        return GetExplorersUnit(GetExplorersSubIdx());
    }
    if (course_idx == YOUNG_INNOVATORS_IDX) {
        return GetYoungInnovatorsUnit(GetYoungInnovatorsSubIdx());
    }
    if (course_idx == FUTURE_LEADERS_IDX) {
        return GetFutureLeadersUnit(GetFutureLeadersSubIdx());
    }
    return GetUnitsForCourse(course_idx);
}

std::string WebSocketControlServer::GetUnitsForCourse(int course_idx) {
    if (course_idx < 1 || course_idx > 5) return "";
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return DEFAULT_UNITS;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_UNITS_PREFIX, course_idx);
    size_t len = 0;
    if (nvs_get_str(nvs, key, nullptr, &len) != ESP_OK || len == 0) {
        nvs_close(nvs);
        return DEFAULT_UNITS;
    }
    char* buf = (char*)malloc(len);
    nvs_get_str(nvs, key, buf, &len);
    std::string units(buf);
    free(buf);
    nvs_close(nvs);
    return units;
}

static void SaveStudentName(const char* name) {
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, NVS_KEY_STUDENT_NAME, name);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void SavePresetMacIdx(int idx) {
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_i32(nvs, NVS_KEY_PRESET_MAC, (int32_t)idx);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void SaveUnitsForCourse(int course_idx, const char* units) {
    if (course_idx < 1 || course_idx > 5) return;
    nvs_handle_t nvs;
    if (nvs_open(OTTO_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_UNITS_PREFIX, course_idx);
    nvs_set_str(nvs, key, units);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// ========== HTTP Handlers ==========

esp_err_t WebSocketControlServer::self_control_page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const size_t html_len = self_control_html_end - self_control_html_start;
    httpd_resp_send(req, self_control_html_start, html_len);
    return ESP_OK;
}

esp_err_t WebSocketControlServer::api_config_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int idx = GetPresetMacIdx();
    std::string name = GetStudentName();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "preset_mac_idx", idx);
    cJSON_AddStringToObject(root, "student_name", name.c_str());
    std::string device_id = SystemInfo::GetMacAddress();
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    std::string custom_mac = GetCustomMacFromNvs();
    cJSON_AddStringToObject(root, "custom_mac", custom_mac.c_str());

    cJSON* preset_macs = cJSON_CreateObject();
    for (int i = 1; i <= kDeviceIdentityPresetCount; i++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", i);
        const char* mac = GetPresetMacByIndex(static_cast<uint8_t>(i));
        cJSON_AddStringToObject(preset_macs, key, mac ? mac : "");
    }
    cJSON_AddItemToObject(root, "preset_macs", preset_macs);

    cJSON* all_units = cJSON_CreateObject();
    for (int i = 1; i <= 5; i++) {
        std::string units = GetUnitsForCourse(i);
        char key[4];
        snprintf(key, sizeof(key), "%d", i);
        cJSON_AddStringToObject(all_units, key, units.c_str());
    }
    cJSON_AddItemToObject(root, "all_units", all_units);

    cJSON_AddNumberToObject(root, "yi_sub", GetYoungInnovatorsSubIdx());
    cJSON_AddNumberToObject(root, "yi_voice", ReadYiVoiceFromNvs());
    cJSON* yi_units = cJSON_CreateObject();
    for (int i = 0; i < YI_SUB_COUNT; i++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", i);
        std::string u = GetYoungInnovatorsUnit(i);
        cJSON_AddStringToObject(yi_units, key, u.c_str());
    }
    cJSON_AddItemToObject(root, "yi_units", yi_units);

    cJSON_AddNumberToObject(root, "ex_sub", GetExplorersSubIdx());
    cJSON_AddNumberToObject(root, "ex_voice", ReadExplorersVoiceFromNvs());
    cJSON* ex_units = cJSON_CreateObject();
    for (int i = 0; i < EXPLORERS_SUB_COUNT; i++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", i);
        std::string u = GetExplorersUnit(i);
        cJSON_AddStringToObject(ex_units, key, u.c_str());
    }
    cJSON_AddItemToObject(root, "ex_units", ex_units);

    cJSON_AddNumberToObject(root, "fl_sub", GetFutureLeadersSubIdx());
    cJSON_AddNumberToObject(root, "fl_voice", ReadFlVoiceFromNvs());
    cJSON* fl_units = cJSON_CreateObject();
    for (int i = 0; i < FL_SUB_COUNT; i++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", i);
        std::string u = GetFutureLeadersUnit(i);
        cJSON_AddStringToObject(fl_units, key, u.c_str());
    }
    cJSON_AddItemToObject(root, "fl_units", fl_units);

    cJSON_AddNumberToObject(root, "ielts_voice", ReadIeltsVoiceFromNvs());
    cJSON_AddNumberToObject(root, "toeic_voice", ReadToeicVoiceFromNvs());

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t WebSocketControlServer::api_config_post_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int total_len = req->content_len;
    if (total_len > 4096) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Too large\"}");
        return ESP_OK;
    }

    char* buf = (char*)malloc(total_len + 1);
    if (!buf) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"OOM\"}");
        return ESP_OK;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }

    int old_idx = GetPresetMacIdx();
    std::string old_mac = SystemInfo::GetMacAddress();
    const uint8_t old_ex_voice = ReadExplorersVoiceFromNvs();
    const uint8_t old_yi_voice = ReadYiVoiceFromNvs();
    const uint8_t old_fl_voice = ReadFlVoiceFromNvs();
    const uint8_t old_ielts_voice = ReadIeltsVoiceFromNvs();
    const uint8_t old_toeic_voice = ReadToeicVoiceFromNvs();
    bool course_changed = false;
    bool identity_changed = false;
    bool explorers_voice_changed = false;
    bool yi_voice_changed = false;
    bool fl_voice_changed = false;
    bool ielts_voice_changed = false;
    bool toeic_voice_changed = false;

    cJSON* name_item = cJSON_GetObjectItem(root, "student_name");
    if (name_item && cJSON_IsString(name_item)) {
        SaveStudentName(name_item->valuestring);
        ESP_LOGI(TAG, "Saved student name: %s", name_item->valuestring);
    }

    int new_idx = old_idx;
    cJSON* idx_item = cJSON_GetObjectItem(root, "preset_mac_idx");
    if (idx_item && cJSON_IsNumber(idx_item)) {
        int idx = idx_item->valueint;
        if (idx >= 0 && idx <= 7) {
            new_idx = idx;
            if (idx != old_idx) {
                course_changed = true;
            }
            SavePresetMacIdx(idx);
            ESP_LOGI(TAG, "Saved preset_mac_idx: %d (was %d)", idx, old_idx);
        }
    }

    cJSON* custom_mac_item = cJSON_GetObjectItem(root, "custom_mac");
    if (custom_mac_item && cJSON_IsString(custom_mac_item)) {
        std::string mac = NormalizeMacString(custom_mac_item->valuestring);
        if (new_idx == kManualCustomMacIndex) {
            if (!IsValidMacString(mac.c_str())) {
                cJSON_Delete(root);
                httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid MAC\"}");
                return ESP_OK;
            }
            SaveCustomMacToWifiNvs(mac.c_str());
            ESP_LOGI(TAG, "Saved custom_mac: %s", mac.c_str());
        } else if (new_idx == 0) {
            if (mac.empty()) {
                SaveCustomMacToWifiNvs("");
            } else if (IsValidMacString(mac.c_str())) {
                SaveCustomMacToWifiNvs(mac.c_str());
                ESP_LOGI(TAG, "Saved custom_mac (custom course): %s", mac.c_str());
            }
        }
    }

    cJSON* ex_voice_item = cJSON_GetObjectItem(root, "ex_voice");
    if (ex_voice_item && cJSON_IsNumber(ex_voice_item)) {
        uint8_t voice = static_cast<uint8_t>(ex_voice_item->valueint);
        if (voice != kExplorersVoiceEnglish) {
            voice = kExplorersVoiceBilingual;
        }
        if (voice != old_ex_voice) {
            explorers_voice_changed = true;
        }
        WriteExplorersVoiceToNvs(voice);
        ESP_LOGI(TAG, "Saved ex_voice: %u (was %u)", (unsigned)voice, (unsigned)old_ex_voice);
    }

    cJSON* yi_voice_item = cJSON_GetObjectItem(root, "yi_voice");
    if (yi_voice_item && cJSON_IsNumber(yi_voice_item)) {
        uint8_t voice = static_cast<uint8_t>(yi_voice_item->valueint);
        if (voice != kYiVoiceEnglish) {
            voice = kYiVoiceVietnamese;
        }
        if (voice != old_yi_voice) {
            yi_voice_changed = true;
        }
        WriteYiVoiceToNvs(voice);
        ESP_LOGI(TAG, "Saved yi_voice: %u (was %u)", (unsigned)voice, (unsigned)old_yi_voice);
    }

    cJSON* fl_voice_item = cJSON_GetObjectItem(root, "fl_voice");
    if (fl_voice_item && cJSON_IsNumber(fl_voice_item)) {
        uint8_t voice = static_cast<uint8_t>(fl_voice_item->valueint);
        if (voice != kFlVoiceEnglish) {
            voice = kFlVoiceVietnamese;
        }
        if (voice != old_fl_voice) {
            fl_voice_changed = true;
        }
        WriteFlVoiceToNvs(voice);
        ESP_LOGI(TAG, "Saved fl_voice: %u (was %u)", (unsigned)voice, (unsigned)old_fl_voice);
    }

    cJSON* ielts_voice_item = cJSON_GetObjectItem(root, "ielts_voice");
    if (ielts_voice_item && cJSON_IsNumber(ielts_voice_item)) {
        uint8_t voice = static_cast<uint8_t>(ielts_voice_item->valueint);
        if (voice != kIeltsVoiceEnglish) {
            voice = kIeltsVoiceVietnamese;
        }
        if (voice != old_ielts_voice) {
            ielts_voice_changed = true;
        }
        WriteIeltsVoiceToNvs(voice);
        ESP_LOGI(TAG, "Saved ielts_voice: %u (was %u)", (unsigned)voice, (unsigned)old_ielts_voice);
    }

    cJSON* toeic_voice_item = cJSON_GetObjectItem(root, "toeic_voice");
    if (toeic_voice_item && cJSON_IsNumber(toeic_voice_item)) {
        uint8_t voice = static_cast<uint8_t>(toeic_voice_item->valueint);
        if (voice != kToeicVoiceEnglish) {
            voice = kToeicVoiceVietnamese;
        }
        if (voice != old_toeic_voice) {
            toeic_voice_changed = true;
        }
        WriteToeicVoiceToNvs(voice);
        ESP_LOGI(TAG, "Saved toeic_voice: %u (was %u)", (unsigned)voice, (unsigned)old_toeic_voice);
    }

    if ((course_changed && CourseUsesMacPool(static_cast<uint8_t>(new_idx))) ||
        (new_idx == static_cast<int>(kDefaultPresetMacIndex) && explorers_voice_changed) ||
        (new_idx == static_cast<int>(kYoungInnovatorsPresetMacIndex) && yi_voice_changed) ||
        (new_idx == static_cast<int>(kFutureLeadersPresetMacIndex) && fl_voice_changed) ||
        (new_idx == static_cast<int>(kIeltsPresetMacIndex) && ielts_voice_changed) ||
        (new_idx == static_cast<int>(kToeicPresetMacIndex) && toeic_voice_changed)) {
        const char* picked = PickAndSaveMacPoolForCourse(static_cast<uint8_t>(new_idx));
        ESP_LOGI(TAG, "MAC pool course %d picked: %s", new_idx, picked ? picked : "(none)");
    }

    std::string new_mac = SystemInfo::GetMacAddress();
    if (course_changed || new_mac != old_mac ||
        (new_idx == static_cast<int>(kDefaultPresetMacIndex) && explorers_voice_changed) ||
        (new_idx == static_cast<int>(kYoungInnovatorsPresetMacIndex) && yi_voice_changed) ||
        (new_idx == static_cast<int>(kFutureLeadersPresetMacIndex) && fl_voice_changed) ||
        (new_idx == static_cast<int>(kIeltsPresetMacIndex) && ielts_voice_changed) ||
        (new_idx == static_cast<int>(kToeicPresetMacIndex) && toeic_voice_changed)) {
        identity_changed = true;
    }

    cJSON* all_units = cJSON_GetObjectItem(root, "all_units");
    if (all_units && cJSON_IsObject(all_units)) {
        for (int i = 1; i <= 5; i++) {
            char key[4];
            snprintf(key, sizeof(key), "%d", i);
            cJSON* u = cJSON_GetObjectItem(all_units, key);
            if (u && cJSON_IsString(u)) {
                SaveUnitsForCourse(i, u->valuestring);
                ESP_LOGI(TAG, "Saved units_%d: %s", i, u->valuestring);
            }
        }
    }

    cJSON* yi_sub = cJSON_GetObjectItem(root, "yi_sub");
    if (yi_sub && cJSON_IsNumber(yi_sub)) {
        SaveYoungInnovatorsSubIdx(yi_sub->valueint);
        ESP_LOGI(TAG, "Saved yi_sub: %d", yi_sub->valueint);
    }

    cJSON* yi_units = cJSON_GetObjectItem(root, "yi_units");
    if (yi_units && cJSON_IsObject(yi_units)) {
        for (int i = 0; i < YI_SUB_COUNT; i++) {
            char key[4];
            snprintf(key, sizeof(key), "%d", i);
            cJSON* u = cJSON_GetObjectItem(yi_units, key);
            if (u && cJSON_IsString(u)) {
                SaveYoungInnovatorsUnit(i, u->valuestring);
                ESP_LOGI(TAG, "Saved yi_u%d: %s", i, u->valuestring);
            }
        }
    }

    cJSON* ex_sub = cJSON_GetObjectItem(root, "ex_sub");
    if (ex_sub && cJSON_IsNumber(ex_sub)) {
        SaveExplorersSubIdx(ex_sub->valueint);
        ESP_LOGI(TAG, "Saved ex_sub: %d", ex_sub->valueint);
    }

    cJSON* ex_units = cJSON_GetObjectItem(root, "ex_units");
    if (ex_units && cJSON_IsObject(ex_units)) {
        for (int i = 0; i < EXPLORERS_SUB_COUNT; i++) {
            char key[4];
            snprintf(key, sizeof(key), "%d", i);
            cJSON* u = cJSON_GetObjectItem(ex_units, key);
            if (u && cJSON_IsString(u)) {
                SaveExplorersUnit(i, u->valuestring);
                ESP_LOGI(TAG, "Saved ex_u%d: %s", i, u->valuestring);
            }
        }
    }

    cJSON* fl_sub = cJSON_GetObjectItem(root, "fl_sub");
    if (fl_sub && cJSON_IsNumber(fl_sub)) {
        SaveFutureLeadersSubIdx(fl_sub->valueint);
        ESP_LOGI(TAG, "Saved fl_sub: %d", fl_sub->valueint);
    }

    cJSON* fl_units = cJSON_GetObjectItem(root, "fl_units");
    if (fl_units && cJSON_IsObject(fl_units)) {
        for (int i = 0; i < FL_SUB_COUNT; i++) {
            char key[4];
            snprintf(key, sizeof(key), "%d", i);
            cJSON* u = cJSON_GetObjectItem(fl_units, key);
            if (u && cJSON_IsString(u)) {
                SaveFutureLeadersUnit(i, u->valuestring);
                ESP_LOGI(TAG, "Saved fl_u%d: %s", i, u->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"success\":true}");
    if (identity_changed) {
        Application::GetInstance().ApplyDeviceIdentity();
    }
    return ESP_OK;
}

esp_err_t WebSocketControlServer::api_robot_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    auto& board = Board::GetInstance();
    cJSON* root = cJSON_CreateObject();
    int volume = 70;
    auto* codec = board.GetAudioCodec();
    if (codec) {
        volume = codec->output_volume();
    }
    cJSON_AddNumberToObject(root, "volume", volume);

    auto* backlight = board.GetBacklight();
    if (backlight) {
        cJSON_AddNumberToObject(root, "brightness", backlight->brightness());
        cJSON_AddBoolToObject(root, "has_brightness", true);
    } else {
        cJSON_AddBoolToObject(root, "has_brightness", false);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_sendstr(req, "{\"volume\":70,\"has_brightness\":false}");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

esp_err_t WebSocketControlServer::api_robot_post_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 1024) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Bad body\"}");
        return ESP_OK;
    }
    char* buf = (char*)malloc(total_len + 1);
    if (!buf) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"OOM\"}");
        return ESP_OK;
    }
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }

    auto& board = Board::GetInstance();
    cJSON* vol = cJSON_GetObjectItem(root, "volume");
    if (vol && cJSON_IsNumber(vol)) {
        int v = vol->valueint;
        v = std::max(0, std::min(100, v));
        auto* codec = board.GetAudioCodec();
        if (codec) {
            codec->SetOutputVolume(v);
            ESP_LOGI(TAG, "Robot volume set to %d", v);
        }
    }
    cJSON* bri = cJSON_GetObjectItem(root, "brightness");
    if (bri && cJSON_IsNumber(bri)) {
        int b = bri->valueint;
        b = std::max(0, std::min(100, b));
        auto* backlight = board.GetBacklight();
        if (backlight) {
            backlight->SetBrightness(static_cast<uint8_t>(b), true);
            ESP_LOGI(TAG, "Robot brightness set to %d", b);
        }
    }
    cJSON_Delete(root);

    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

// ========== Server Start ==========

bool WebSocketControlServer::Start(int port) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 16;

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = nullptr,
        .is_websocket = true
    };

    httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = self_control_page_handler,
        .user_ctx = nullptr,
        .is_websocket = false
    };

    httpd_uri_t api_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = api_config_get_handler,
        .user_ctx = nullptr,
        .is_websocket = false
    };

    httpd_uri_t api_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = api_config_post_handler,
        .user_ctx = nullptr,
        .is_websocket = false
    };

    httpd_uri_t api_robot_get_uri = {
        .uri = "/api/robot",
        .method = HTTP_GET,
        .handler = api_robot_get_handler,
        .user_ctx = nullptr,
        .is_websocket = false
    };

    httpd_uri_t api_robot_post_uri = {
        .uri = "/api/robot",
        .method = HTTP_POST,
        .handler = api_robot_post_handler,
        .user_ctx = nullptr,
        .is_websocket = false
    };

    if (httpd_start(&server_handle_, &config) == ESP_OK) {
        httpd_register_uri_handler(server_handle_, &ws_uri);
        httpd_register_uri_handler(server_handle_, &page_uri);
        httpd_register_uri_handler(server_handle_, &api_get_uri);
        httpd_register_uri_handler(server_handle_, &api_post_uri);
        httpd_register_uri_handler(server_handle_, &api_robot_get_uri);
        httpd_register_uri_handler(server_handle_, &api_robot_post_uri);
        ESP_LOGI(TAG, "WebSocket + Self-Control server started on port %d", port);
        return true;
    }

    ESP_LOGE(TAG, "Failed to start WebSocket server");
    return false;
}

void WebSocketControlServer::Stop() {
    if (server_handle_) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        clients_.clear();
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
}

void WebSocketControlServer::HandleMessage(httpd_req_t *req, const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        ESP_LOGE(TAG, "Invalid message: data is null or len is 0");
        return;
    }
    
    if (len > 4096) {
        ESP_LOGE(TAG, "Message too long: %zu bytes", len);
        return;
    }
    
    char* temp_buf = (char*)malloc(len + 1);
    if (temp_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return;
    }
    memcpy(temp_buf, data, len);
    temp_buf[len] = '\0';
    
    cJSON* root = cJSON_Parse(temp_buf);
    free(temp_buf);
    
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    // 支持两种格式：
    // 1. 完整格式：{"type":"mcp","payload":{...}}
    // 2. 简化格式：直接是MCP payload对象
    
    cJSON* payload = nullptr;
    cJSON* type = cJSON_GetObjectItem(root, "type");
    
    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        payload = cJSON_GetObjectItem(root, "payload");
        if (payload != nullptr) {
            cJSON_DetachItemViaPointer(root, payload);
            McpServer::GetInstance().ParseMessage(payload);
            cJSON_Delete(payload); 
        }
    } else {
        payload = cJSON_Duplicate(root, 1);
        if (payload != nullptr) {
            McpServer::GetInstance().ParseMessage(payload);
            cJSON_Delete(payload);
        }
    }
    
    if (payload == nullptr) {
        ESP_LOGE(TAG, "Invalid message format or failed to parse");
    }

    cJSON_Delete(root);
}

void WebSocketControlServer::AddClient(httpd_req_t *req) {
    int sock_fd = httpd_req_to_sockfd(req);
    if (clients_.find(sock_fd) == clients_.end()) {
        clients_[sock_fd] = req;
        ESP_LOGI(TAG, "Client connected: %d (total: %zu)", sock_fd, clients_.size());
    }
}

void WebSocketControlServer::RemoveClient(httpd_req_t *req) {
    int sock_fd = httpd_req_to_sockfd(req);
    clients_.erase(sock_fd);
    ESP_LOGI(TAG, "Client disconnected: %d (total: %zu)", sock_fd, clients_.size());
}

size_t WebSocketControlServer::GetClientCount() const {
    return clients_.size();
}

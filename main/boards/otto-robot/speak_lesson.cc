#include "speak_lesson.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <esp_random.h>

#include "application.h"
#include "board.h"
#include <http.h>
#include "lvgl_display.h"
#include "lvgl_image.h"

#include <stdexcept>
#include "direct_ai_config.h"
#include "device_identity_presets.h"
#include "settings.h"
#include "websocket_control_server.h"

#define TAG "SpeakLesson"
#define SPEAK_NVS "speak"

namespace {

constexpr const char* kIntroSay =
    "Mình có nhiệm vụ dạy tiếng Anh cho bạn đây, mình bắt đầu học nhé!";

constexpr const char* kDoneSay =
    "Hôm nay mình hoàn thành chủ đề này rồi. Bạn muốn học lại từ đầu, học chủ đề tiếp theo, hay nghỉ nói chuyện?";

constexpr const char* kDoneSayLast =
    "Hôm nay mình hoàn thành chủ đề cuối rồi. Bạn muốn học lại từ đầu hay nghỉ nói chuyện?";

constexpr const char* kXiaozhiPrompt =
    "Khi tool self.speak.status cho on=true, bạn là giáo viên luyện NÓI tiếng Anh.\n"
    "Lượt ĐẦU (phase=intro / need_intro): CHỈ nói đúng field say, KHÔNG show, KHÔNG dạy từ vựng. Chờ trẻ đáp.\n"
    "Lượt sau: gọi self.speak.card rồi self.speak.show, chỉ nói đúng field teacher, chờ trẻ nói mic.\n"
    "Trẻ nói bất kỳ gì (sai, ngọng, tiếng Việt): khen ngắn \"Đúng rồi!\", gọi self.speak.next rồi show thẻ mới.\n"
    "Không chữa phát âm, không bịa từ ngoài thẻ. Không có field explain: chỉ nói teacher, không giảng nghĩa.\n"
    "Có explain: sau teacher nói đúng explain.vi (1-2 câu), không bịa thêm.\n"
    "phase=done / finished=true: HẾT CHỦ ĐỀ. CHỈ nói đúng field say. KHÔNG next, KHÔNG show, KHÔNG daily. Chờ trẻ.\n"
    "Học lại / từ đầu: self.speak.start index=0 cùng unit hiện tại.\n"
    "Chủ đề tiếp / unit sau: self.speak.start index=0 unit=next_unit (nếu có next_unit).\n"
    "Nghỉ / không / về giao tiếp: self.speak.off hoặc self.speak.daily.\n"
    "Đang giao tiếp mà trẻ muốn học / đổi chủ đề: self.speak.start với unit trẻ chọn (u01–u36). Tự do chọn unit, kể cả unit đã học.\n"
    "Đổi chế độ đóng phiên cũ, kết nối lại và chào.\n"
    "Khi on=false: trò chuyện đời thường, không dẫn bài.";

constexpr const char* kXiaozhiPromptEn =
    "When tool self.speak.status returns on=true, you are an English speaking teacher.\n"
    "FIRST turn (phase=intro / need_intro): say ONLY the \"say\" field. Do NOT show. Do NOT teach vocabulary. Wait for the child.\n"
    "Later turns: call self.speak.card then self.speak.show, say ONLY the \"teacher\" field, then wait.\n"
    "Whatever the child says: praise \"Đúng rồi!\", call self.speak.next, then show the new card.\n"
    "No explain field: say teacher only, do not teach meaning. If explain exists: after teacher, say explain.vi only.\n"
    "phase=done / finished=true: say ONLY \"say\". Do NOT next/show/daily. Wait.\n"
    "Restart this topic: self.speak.start index=0 same unit.\n"
    "Next topic: self.speak.start index=0 unit=next_unit (if next_unit is set).\n"
    "Stop / daily chat: self.speak.off or self.speak.daily.\n"
    "Child may pick any unit u01–u36, including ones already done: self.speak.start with that unit.\n"
    "When on=false: chat normally, do not lead the lesson.";

constexpr const char* kDirectPrompt =
    "Bạn đang ở CHẾ ĐỘ HỌC nói tiếng Anh.\n"
    "Lượt ĐẦU: gọi self.speak.card. phase=intro: CHỈ nói field say, không show/next. Chờ trẻ.\n"
    "Lượt sau: card + show, nói teacher. Có explain thì nói thêm explain.vi. Không explain thì đừng giảng nghĩa.\n"
    "Trẻ nói gì cũng khen \"Đúng rồi!\", next rồi show.\n"
    "phase=done: CHỈ nói field say. Học lại: start index=0 cùng unit. Tiếp: start index=0 unit=next_unit. Nghỉ: self.speak.off.\n"
    "Được chọn tự do unit u01–u36, kể cả unit đã học. Không nhảy giao tiếp khi hết bài — phải hỏi trẻ.";

std::string NextUnitId(const std::string& unit) {
    int n = 0;
    if (unit.size() >= 2 && (unit[0] == 'u' || unit[0] == 'U')) {
        n = atoi(unit.c_str() + 1);
    }
    if (n < 1 || n >= 36) {
        return "";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "u%02d", n + 1);
    return std::string(buf);
}

std::string TrimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::string MakeUuidV4() {
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    char uuid_str[37];
    snprintf(uuid_str, sizeof(uuid_str),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5], uuid[6], uuid[7],
        uuid[8], uuid[9], uuid[10], uuid[11],
        uuid[12], uuid[13], uuid[14], uuid[15]);
    return std::string(uuid_str);
}

bool SameMac(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a && *b) {
        if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

bool IsDailyPoolMac(const std::string& mac) {
    if (mac.empty()) {
        return false;
    }
    for (int i = 0; i < kDailyChatMacPoolCount; ++i) {
        if (SameMac(kDailyChatMacPool[i], mac.c_str())) {
            return true;
        }
    }
    return false;
}

const char* PickDailyPoolMac(const std::string& current) {
    if (kDailyChatMacPoolCount <= 0) {
        return nullptr;
    }
    int i = static_cast<int>(esp_random() % static_cast<uint32_t>(kDailyChatMacPoolCount));
    if (kDailyChatMacPoolCount > 1 && SameMac(kDailyChatMacPool[i], current.c_str())) {
        i = (i + 1) % kDailyChatMacPoolCount;
    }
    return kDailyChatMacPool[i];
}

/** Locally administered unicast MAC, never World Edu / old daily pools. */
std::string MakeLocalMac() {
    uint8_t m[6];
    esp_fill_random(m, sizeof(m));
    m[0] = static_cast<uint8_t>((m[0] & 0xFE) | 0x02);
    char buf[18];
    snprintf(buf, sizeof(buf), "02:%02x:%02x:%02x:%02x:%02x", m[1], m[2], m[3], m[4], m[5]);
    return std::string(buf);
}

const char* SlotName(uint8_t slot) {
    if (slot == 1) {
        return "daily";
    }
    if (slot == 2) {
        return "custom";
    }
    return "teach";
}

std::string NormalizeTeachMac(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (unsigned char c : in) {
        if (c == ' ' || c == '\t') {
            continue;
        }
        if (c == '-') {
            s.push_back(':');
        } else {
            s.push_back(static_cast<char>(tolower(c)));
        }
    }
    unsigned a, b, c, d, e, f;
    if (s.size() != 17 ||
        sscanf(s.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &a, &b, &c, &d, &e, &f) != 6) {
        return "";
    }
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a, b, c, d, e, f);
    return std::string(buf);
}

std::string NormalizeTeachBackend(const std::string& name) {
    if (name == "gemini" || name == "openai" || name == "grok" || name == "xiaozhi") {
        return name;
    }
    return "xiaozhi";
}

std::string MaskTeachKey(const std::string& key) {
    if (key.empty()) {
        return "";
    }
    if (key.size() <= 10) {
        return "…" + key.substr(key.size() - 2);
    }
    return key.substr(0, 3) + "…" + key.substr(key.size() - 4);
}

std::string TrimClientId(std::string id) {
    while (!id.empty() && isspace(static_cast<unsigned char>(id.front()))) {
        id.erase(id.begin());
    }
    while (!id.empty() && isspace(static_cast<unsigned char>(id.back()))) {
        id.pop_back();
    }
    if (id.size() > 64) {
        id.resize(64);
    }
    return id;
}

int CardCount(cJSON* root) {
    cJSON* cards = cJSON_GetObjectItem(root, "cards");
    return cJSON_IsArray(cards) ? cJSON_GetArraySize(cards) : 0;
}

cJSON* CardAt(cJSON* root, int index) {
    cJSON* cards = cJSON_GetObjectItem(root, "cards");
    if (!cJSON_IsArray(cards)) {
        return nullptr;
    }
    return cJSON_GetArrayItem(cards, index);
}

}  // namespace

SpeakLesson& SpeakLesson::Get() {
    static SpeakLesson inst;
    inst.LoadNvs();
    return inst;
}

void SpeakLesson::LoadNvs() {
    if (loaded_nvs_) {
        return;
    }
    Settings s(SPEAK_NVS, false);
    on_ = s.GetBool("on", false);
    base_ = TrimSlash(s.GetString("base", "http://192.168.100.161:8765"));
    if (base_.empty()) {
        base_ = "http://192.168.100.161:8765";
    }
    level_ = s.GetString("lv", "a1");
    for (char& c : level_) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    unit_ = s.GetString("u", "u01");
    index_ = s.GetInt("i", 0);
    age_ = s.GetInt("age", 0);
    dob_ = s.GetString("dob", "");
    likes_ = s.GetString("likes", "");
    mac_ = s.GetString("mac", "");
    uuid_ = s.GetString("uuid", "");
    dai_ = NormalizeTeachBackend(s.GetString("dai", "xiaozhi"));
    akey_ = s.GetString("akey", "");
    amod_ = s.GetString("amod", "");
    avoi_ = s.GetString("avoi", "");
    ains_ = s.GetString("ains", "");
    ddai_ = NormalizeTeachBackend(s.GetString("ddai", "xiaozhi"));
    dkey_ = s.GetString("dkey", "");
    dmod_ = s.GetString("dmod", "");
    dvoi_ = s.GetString("dvoi", "");
    dins_ = s.GetString("dins", "");
    sub_robot_ = s.GetBool("sub_r", false);
    sub_stt_ = s.GetBool("sub_u", false);
    style_new_ = s.GetBool("es", false);
    dmac_ = s.GetString("dmac", "");
    duid_ = s.GetString("duid", "");
    cmac_ = s.GetString("cmac", "");
    cuid_ = s.GetString("cuid", "");
    nvs_handle_t nvs = 0;
    if (nvs_open(SPEAK_NVS, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t ns = 0;
        if (nvs_get_u8(nvs, "ns", &ns) == ESP_OK && ns <= 2) {
            new_slot_ = ns;
        }
        uint8_t ex = 0;
        if (nvs_get_u8(nvs, "exl", &ex) == ESP_OK && ex <= 2) {
            explain_level_ = ex;
        }
        nvs_close(nvs);
    }
    loaded_nvs_ = true;
}

void SpeakLesson::SaveNvs() {
    nvs_handle_t nvs = 0;
    if (nvs_open(SPEAK_NVS, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    nvs_set_u8(nvs, "on", on_ ? 1 : 0);
    nvs_set_str(nvs, "base", base_.c_str());
    nvs_set_str(nvs, "lv", level_.c_str());
    nvs_set_str(nvs, "u", unit_.c_str());
    nvs_set_i32(nvs, "i", index_);
    nvs_set_i32(nvs, "age", age_);
    nvs_set_str(nvs, "dob", dob_.c_str());
    nvs_set_str(nvs, "likes", likes_.c_str());
    nvs_set_str(nvs, "mac", mac_.c_str());
    nvs_set_str(nvs, "uuid", uuid_.c_str());
    nvs_set_str(nvs, "dai", dai_.c_str());
    nvs_set_str(nvs, "akey", akey_.c_str());
    nvs_set_str(nvs, "amod", amod_.c_str());
    nvs_set_str(nvs, "avoi", avoi_.c_str());
    nvs_set_str(nvs, "ains", ains_.c_str());
    nvs_set_str(nvs, "ddai", ddai_.c_str());
    nvs_set_str(nvs, "dkey", dkey_.c_str());
    nvs_set_str(nvs, "dmod", dmod_.c_str());
    nvs_set_str(nvs, "dvoi", dvoi_.c_str());
    nvs_set_str(nvs, "dins", dins_.c_str());
    nvs_set_u8(nvs, "sub_r", sub_robot_ ? 1 : 0);
    nvs_set_u8(nvs, "exl", explain_level_);
    nvs_set_u8(nvs, "sub_u", sub_stt_ ? 1 : 0);
    nvs_set_u8(nvs, "es", style_new_ ? 1 : 0);
    nvs_set_u8(nvs, "ns", new_slot_);
    nvs_set_str(nvs, "dmac", dmac_.c_str());
    nvs_set_str(nvs, "duid", duid_.c_str());
    nvs_set_str(nvs, "cmac", cmac_.c_str());
    nvs_set_str(nvs, "cuid", cuid_.c_str());
    nvs_commit(nvs);
    nvs_close(nvs);
}

bool SpeakLesson::IsOn() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), on_;
}

bool SpeakLesson::UsesNewIdentity() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), style_new_;
}

bool SpeakLesson::UsesTeachIdentity() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    return self.style_new_ && self.new_slot_ == 0;
}

bool SpeakLesson::UsesDailyIdentity() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    return self.style_new_ && self.new_slot_ == 1;
}

const char* SpeakLesson::ActiveAiBackendName() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (!self.style_new_) {
        return "xiaozhi";
    }
    if (self.new_slot_ == 1) {
        return self.ddai_.c_str();
    }
    if (self.new_slot_ == 2) {
        return "xiaozhi";
    }
    return self.dai_.c_str();
}

std::string SpeakLesson::ActiveAiKey() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (self.style_new_ && self.new_slot_ == 1) {
        return self.dkey_;
    }
    if (self.style_new_ && self.new_slot_ == 0) {
        return self.akey_;
    }
    return "";
}

std::string SpeakLesson::ActiveAiModel() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (self.style_new_ && self.new_slot_ == 1) {
        return self.dmod_;
    }
    if (self.style_new_ && self.new_slot_ == 0) {
        return self.amod_;
    }
    return "";
}

std::string SpeakLesson::ActiveAiVoice() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (self.style_new_ && self.new_slot_ == 1) {
        return self.dvoi_;
    }
    if (self.style_new_ && self.new_slot_ == 0) {
        return self.avoi_;
    }
    return "";
}

std::string SpeakLesson::ActiveAiInstructions() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (self.style_new_ && self.new_slot_ == 1) {
        return self.dins_;
    }
    if (self.style_new_ && self.new_slot_ == 0) {
        return self.ains_;
    }
    return "";
}

const char* SpeakLesson::EnglishStyleName() const {
    return UsesNewIdentity() ? "new" : "old";
}

const char* SpeakLesson::EnglishSlotName() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    return SlotName(self.new_slot_);
}

std::string SpeakLesson::ActiveDeviceMac() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (!self.style_new_) {
        return "";
    }
    if (self.new_slot_ == 1) {
        return self.dmac_;
    }
    if (self.new_slot_ == 2) {
        return self.cmac_;
    }
    return self.mac_;
}

std::string SpeakLesson::ActiveClientId() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (!self.style_new_) {
        return "";
    }
    if (self.new_slot_ == 1) {
        return self.duid_;
    }
    if (self.new_slot_ == 2) {
        return self.cuid_;
    }
    return self.uuid_;
}

void SpeakLesson::EnsureActiveClientId() {
    LoadNvs();
    if (!style_new_) {
        return;
    }
    bool dirty = false;
    if (new_slot_ == 1) {
        if (ddai_ == "xiaozhi") {
            if (dmac_.empty() || !IsDailyPoolMac(dmac_)) {
                const char* mac = PickDailyPoolMac(dmac_);
                if (mac) {
                    dmac_ = mac;
                    dirty = true;
                }
            }
        } else if (dmac_.empty()) {
            dmac_ = MakeLocalMac();
            dirty = true;
        }
        if (duid_.empty()) {
            duid_ = MakeUuidV4();
            dirty = true;
        }
    } else if (new_slot_ == 2) {
        if (cuid_.empty()) {
            cuid_ = MakeUuidV4();
            dirty = true;
        }
    } else if (uuid_.empty()) {
        uuid_ = MakeUuidV4();
        dirty = true;
    }
    if (dirty) {
        SaveNvs();
    }
}

std::string SpeakLesson::SetEnglishStyle(bool is_new, int slot) {
    LoadNvs();
    if (slot >= 0 && slot <= 2) {
        new_slot_ = static_cast<uint8_t>(slot);
    } else if (is_new) {
        new_slot_ = 1;  // kiểu mới mặc định = giao tiếp hằng ngày
    }
    style_new_ = is_new;
    if (!is_new) {
        on_ = false;
        intro_pending_ = false;
        intro_hold_ = false;
        intro_ready_ = false;
    } else {
        if (new_slot_ != 0) {
            on_ = false;
            intro_pending_ = false;
            intro_hold_ = false;
            intro_ready_ = false;
        }
        EnsureActiveClientId();
    }
    SaveNvs();
    ESP_LOGI(TAG, "English style=%s slot=%s", EnglishStyleName(), EnglishSlotName());
    SwitchIdentity();
    return StatusJson();
}

std::string SpeakLesson::SetNewDaily() {
    return GoDaily();
}

void SpeakLesson::RerollDailyXiaozhiIdentity() {
    LoadNvs();
    const char* mac = PickDailyPoolMac(dmac_);
    if (mac) {
        dmac_ = mac;
    }
    duid_ = MakeUuidV4();
    ESP_LOGI(TAG, "Daily Xiaozhi pool Device-Id=%s Client-Id=%s", dmac_.c_str(), duid_.c_str());
}

void SpeakLesson::ApplyDailyPoolOnBoot() {
    LoadNvs();
    if (!style_new_) {
        return;
    }
    teach_armed_ = false;
    identity_pending_ = false;
    done_ask_ = false;
    on_ = false;
    new_slot_ = 1;
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    if (ddai_ == "xiaozhi") {
        RerollDailyXiaozhiIdentity();
    }
    SaveNvs();
    ESP_LOGI(TAG, "Boot: daily chat Device-Id=%s backend=%s", dmac_.c_str(), ddai_.c_str());
}

void SpeakLesson::ParkLessonToDaily() {
    LoadNvs();
    teach_armed_ = false;
    identity_pending_ = false;
    done_ask_ = false;
    on_ = false;
    new_slot_ = 1;
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    SaveNvs();
    ESP_LOGI(TAG, "Parked to daily chat for next wake, Device-Id=%s backend=%s",
             dmac_.c_str(), ddai_.c_str());
}

void SpeakLesson::EnsureDailyOnWake() {
    LoadNvs();
    if (!style_new_) {
        return;
    }
    if (teach_armed_ || (done_ask_ && on_)) {
        ESP_LOGI(TAG, "Wake: keep teaching (lesson or ask-restart)");
        return;
    }
    if (new_slot_ == 1 && !on_) {
        return;
    }
    ParkLessonToDaily();
}

std::string SpeakLesson::GoDaily(const std::string& backend) {
    LoadNvs();
    if (!backend.empty()) {
        ddai_ = NormalizeTeachBackend(backend);
        if (ddai_ != "xiaozhi" && dkey_.empty()) {
            return "{\"success\":false,\"error\":\"Chưa nhập API key Gemini cho giao tiếp hằng ngày\"}";
        }
    }
    const bool already = style_new_ && new_slot_ == 1 && !on_;
    style_new_ = true;
    new_slot_ = 1;
    on_ = false;
    teach_armed_ = false;
    done_ask_ = false;
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    if (ddai_ == "xiaozhi") {
        RerollDailyXiaozhiIdentity();
    } else {
        if (dmac_.empty()) {
            dmac_ = MakeLocalMac();
        }
        if (duid_.empty()) {
            duid_ = MakeUuidV4();
        }
    }
    SaveNvs();
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetPreviewImage(nullptr);
    }
    ESP_LOGI(TAG, "Daily chat Device-Id=%s backend=%s", dmac_.c_str(), ddai_.c_str());
    if (ddai_ != "xiaozhi" && InLiveVoiceSession()) {
        const auto want = DirectAiConfig::LoadBackend();
        const auto have = DirectAiConfig::RunningBackend();
        if (want == have && want != DirectAiBackend::kOff) {
            ESP_LOGI(TAG, "Stay on %s daily, no Xiaozhi hop", ddai_.c_str());
            return StatusJson();
        }
    }
    if (already && backend.empty() && ddai_ != "xiaozhi") {
        ESP_LOGI(TAG, "Daily chat already active, skip reconnect");
        return StatusJson();
    }
    SwitchIdentity();
    return StatusJson();
}

std::string SpeakLesson::SetDailyIdentity(const std::string& mac, const std::string& client_id,
                                          const std::string& ai_backend, const std::string& key, bool clear_key,
                                          const std::string& model, const std::string& voice,
                                          const std::string& instructions) {
    LoadNvs();
    if (!mac.empty()) {
        std::string norm = NormalizeTeachMac(mac);
        if (norm.empty()) {
            return "{\"success\":false,\"error\":\"MAC giao tiếp hằng ngày phải dạng aa:bb:cc:dd:ee:ff\"}";
        }
        dmac_ = norm;
    }
    if (!client_id.empty()) {
        duid_ = TrimClientId(client_id);
    }
    if (!ai_backend.empty()) {
        ddai_ = NormalizeTeachBackend(ai_backend);
    }
    if (clear_key) {
        dkey_.clear();
    } else if (!key.empty()) {
        dkey_ = key.substr(0, 300);
    }
    if (!model.empty()) {
        dmod_ = model.substr(0, 80);
    }
    if (!voice.empty()) {
        dvoi_ = voice.substr(0, 40);
    }
    if (!instructions.empty()) {
        dins_ = instructions.substr(0, 400);
    }
    if (ddai_ != "xiaozhi" && dkey_.empty()) {
        return "{\"success\":false,\"error\":\"Chưa nhập API key Gemini cho giao tiếp hằng ngày\"}";
    }
    if (ddai_ == "xiaozhi") {
        if (dmac_.empty() || !IsDailyPoolMac(dmac_)) {
            RerollDailyXiaozhiIdentity();
        } else if (duid_.empty()) {
            duid_ = MakeUuidV4();
        }
    } else {
        if (dmac_.empty()) {
            dmac_ = MakeLocalMac();
        }
        if (duid_.empty()) {
            duid_ = MakeUuidV4();
        }
    }
    style_new_ = true;
    new_slot_ = 1;
    on_ = false;
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    SaveNvs();
    ESP_LOGI(TAG, "Daily identity Device-Id=%s backend=%s", dmac_.c_str(), ddai_.c_str());
    SwitchIdentity();
    return StatusJson();
}

std::string SpeakLesson::SetNewCustom(const std::string& mac, const std::string& client_id) {
    LoadNvs();
    if (!mac.empty()) {
        std::string norm = NormalizeTeachMac(mac);
        if (norm.empty()) {
            return "{\"success\":false,\"error\":\"MAC tự cấu hình kiểu mới phải dạng aa:bb:cc:dd:ee:ff\"}";
        }
        cmac_ = norm;
    }
    if (cmac_.empty()) {
        return "{\"success\":false,\"error\":\"Chưa nhập MAC Xiaozhi cho tự cấu hình kiểu mới\"}";
    }
    if (!client_id.empty()) {
        std::string id = client_id;
        while (!id.empty() && isspace(static_cast<unsigned char>(id.front()))) {
            id.erase(id.begin());
        }
        while (!id.empty() && isspace(static_cast<unsigned char>(id.back()))) {
            id.pop_back();
        }
        if (id.size() > 64) {
            id.resize(64);
        }
        cuid_ = id;
    }
    if (cuid_.empty()) {
        cuid_ = MakeUuidV4();
    }
    style_new_ = true;
    new_slot_ = 2;
    on_ = false;
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    SaveNvs();
    ESP_LOGI(TAG, "New-style custom Device-Id=%s", cmac_.c_str());
    SwitchIdentity();
    return StatusJson();
}

std::string SpeakLesson::BaseUrl() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), base_;
}

std::string SpeakLesson::Level() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), level_;
}

std::string SpeakLesson::UnitId() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), unit_;
}

int SpeakLesson::Index() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), index_;
}

std::string SpeakLesson::DeviceMac() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), mac_;
}

std::string SpeakLesson::ClientId() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), uuid_;
}

std::string SpeakLesson::AiBackendName() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), dai_;
}

std::string SpeakLesson::AiKey() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), akey_;
}

std::string SpeakLesson::AiModel() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), amod_;
}

std::string SpeakLesson::AiVoice() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), avoi_;
}

std::string SpeakLesson::AiInstructions() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), ains_;
}

void SpeakLesson::EnsureClientId() {
    LoadNvs();
    if (!uuid_.empty()) {
        return;
    }
    uuid_ = MakeUuidV4();
    SaveNvs();
}

void SpeakLesson::SwitchIdentity() {
    identity_pending_ = false;
    DirectAiConfig::BumpGeneration();
    Application::GetInstance().ApplyDeviceIdentity();
}

bool SpeakLesson::InLiveVoiceSession() const {
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
        state == kDeviceStateConnecting) {
        return true;
    }
    return app.IsAudioChannelOpened();
}

std::string SpeakLesson::IdentityReadyError() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (self.dai_ == "xiaozhi") {
        if (self.mac_.empty()) {
            return "Chưa dán MAC ID dạy học (tách biệt World Edu / giao tiếp).";
        }
    } else if (self.akey_.empty()) {
        return "Chưa nhập API key Gemini dạy học (tách biệt cài đặt cũ).";
    }
    return "";
}

bool SpeakLesson::ShowRobotSubtitle() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), sub_robot_;
}

bool SpeakLesson::ShowSttSubtitle() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), sub_stt_;
}

std::string SpeakLesson::SetSubtitles(bool robot, bool stt) {
    LoadNvs();
    sub_robot_ = robot;
    sub_stt_ = stt;
    SaveNvs();
    auto* display = Board::GetInstance().GetDisplay();
    if (display) {
        display->RefreshChatOverlay();
    }
    return StatusJson();
}

std::string SpeakLesson::SetIdentity(const std::string& mac, const std::string& client_id,
                                    const std::string& ai_backend, const std::string& key, bool clear_key,
                                    const std::string& model, const std::string& voice,
                                    const std::string& instructions) {
    LoadNvs();
    if (!mac.empty()) {
        std::string norm = NormalizeTeachMac(mac);
        if (norm.empty()) {
            return "{\"success\":false,\"error\":\"MAC ID dạy học không đúng dạng aa:bb:cc:dd:ee:ff\"}";
        }
        mac_ = norm;
    }
    if (!client_id.empty()) {
        uuid_ = TrimClientId(client_id);
    }
    if (!ai_backend.empty()) {
        dai_ = NormalizeTeachBackend(ai_backend);
    }
    if (clear_key) {
        akey_.clear();
    } else if (!key.empty()) {
        akey_ = key.substr(0, 300);
    }
    if (!model.empty()) {
        amod_ = model.substr(0, 80);
    }
    if (!voice.empty()) {
        avoi_ = voice.substr(0, 40);
    }
    ains_ = instructions.substr(0, 400);
    SaveNvs();
    ESP_LOGI(TAG, "Saved teach account backend=%s (daily until self.speak.start)", dai_.c_str());
    return StatusJson();
}

std::string SpeakLesson::SetExplainLevel(int level) {
    LoadNvs();
    if (level < 0) {
        level = 0;
    }
    if (level > 2) {
        level = 2;
    }
    explain_level_ = static_cast<uint8_t>(level);
    SaveNvs();
    ESP_LOGI(TAG, "Explain level=%u", (unsigned)explain_level_);
    return StatusJson();
}

void SpeakLesson::SetBaseUrl(const std::string& url) {
    LoadNvs();
    base_ = TrimSlash(url);
    cached_json_.clear();
    cached_key_.clear();
    SaveNvs();
}

std::string SpeakLesson::JoinUrl(const std::string& path) const {
    if (path.empty()) {
        return base_;
    }
    if (path[0] == '/') {
        return base_ + path;
    }
    return base_ + "/" + path;
}

std::string SpeakLesson::FetchText(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return "";
    }
    ESP_LOGI(TAG, "GET %s", url.c_str());
    auto http = network->CreateHttp(4);
    if (!http) {
        ESP_LOGW(TAG, "GET CreateHttp failed %s", url.c_str());
        return "";
    }
    // ReadAll() deadlocks in HttpClient when the body exceeds 8KB (u01.json is ~8.6KB):
    // OnTcpData holds mutex_ while waiting to drain chunks, ReadAll waits on the same mutex.
    http->SetTimeout(4000);
    http->SetKeepAlive(false);
    http->SetHeader("Connection", "close");
    http->SetHeader("Accept", "application/json,*/*");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "GET fail %s", url.c_str());
        return "";
    }
    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s status %d", url.c_str(), status);
        http->Close();
        return "";
    }
    std::string body;
    body.reserve(4096);
    char buf[512];
    while (body.size() < 64 * 1024) {
        int n = http->Read(buf, sizeof(buf));
        if (n < 0) {
            ESP_LOGW(TAG, "GET read timeout %s (%u bytes)", url.c_str(), (unsigned)body.size());
            break;
        }
        if (n == 0) {
            break;
        }
        body.append(buf, static_cast<size_t>(n));
    }
    http->Close();
    ESP_LOGI(TAG, "GET ok %s (%u bytes)", url.c_str(), (unsigned)body.size());
    return body;
}

bool SpeakLesson::EnsureUnitLoaded() {
    LoadNvs();
    std::string key = level_ + "/" + unit_;
    if (cached_key_ == key && !cached_json_.empty()) {
        return true;
    }
    std::string url = JoinUrl("speak/" + level_ + "/" + unit_ + ".json");
    std::string body = FetchText(url);
    if (body.empty()) {
        ESP_LOGW(TAG, "empty unit json %s", url.c_str());
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
        return false;
    }
    cJSON_Delete(root);
    cached_json_ = std::move(body);
    cached_key_ = key;
    return true;
}

std::string SpeakLesson::CardJsonAt(int index, bool with_teacher) {
    if (!EnsureUnitLoaded()) {
        return "{\"error\":\"Khong tai duoc JSON. Bat python http.server 8765 tren may, kiem tra dia chi bai hoc.\"}";
    }
    cJSON* root = cJSON_Parse(cached_json_.c_str());
    if (!root) {
        return "{\"error\":\"JSON loi\"}";
    }
    int total = CardCount(root);
    if (total <= 0) {
        cJSON_Delete(root);
        return "{\"error\":\"Unit khong co the\"}";
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= total) {
        index = total - 1;
    }
    cJSON* card = CardAt(root, index);
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "on", on_);
    cJSON_AddStringToObject(out, "level", level_.c_str());
    cJSON_AddStringToObject(out, "unit", unit_.c_str());
    cJSON_AddNumberToObject(out, "index", index);
    cJSON_AddNumberToObject(out, "total", total);
    if (cJSON_IsObject(card)) {
        auto str = [&](const char* k) {
            cJSON* it = cJSON_GetObjectItem(card, k);
            return cJSON_IsString(it) ? it->valuestring : "";
        };
        const char* id = str("id");
        const char* type = str("type");
        const char* en = str("en");
        const char* vi = str("vi");
        const char* teacher = str("teacher");
        std::string image = JoinUrl(std::string("speak/") + level_ + "/" + unit_ + "/" + id + ".jpg");
        cJSON_AddStringToObject(out, "id", id);
        cJSON_AddStringToObject(out, "type", type);
        cJSON_AddStringToObject(out, "en", en);
        cJSON_AddStringToObject(out, "vi", vi);
        if (with_teacher) {
            cJSON_AddStringToObject(out, "teacher", teacher);
        }
        cJSON_AddStringToObject(out, "image", image.c_str());
        if (explain_level_ > 0) {
            cJSON* explain = cJSON_GetObjectItem(card, "explain");
            if (cJSON_IsObject(explain)) {
                const char* want = (explain_level_ >= 2) ? "detailed" : "basic";
                cJSON* block = cJSON_GetObjectItem(explain, want);
                if (!cJSON_IsObject(block) && explain_level_ >= 2) {
                    block = cJSON_GetObjectItem(explain, "basic");
                }
                if (cJSON_IsObject(block)) {
                    cJSON* vi_e = cJSON_GetObjectItem(block, "vi");
                    cJSON* en_e = cJSON_GetObjectItem(block, "en");
                    if (cJSON_IsString(vi_e) || cJSON_IsString(en_e)) {
                        cJSON* wrap = cJSON_CreateObject();
                        if (cJSON_IsString(vi_e)) {
                            cJSON_AddStringToObject(wrap, "vi", vi_e->valuestring);
                        }
                        if (cJSON_IsString(en_e)) {
                            cJSON_AddStringToObject(wrap, "en", en_e->valuestring);
                        }
                        cJSON_AddItemToObject(out, "explain", wrap);
                    }
                }
            }
        }
        cJSON_AddBoolToObject(out, "done", false);
        cJSON_AddBoolToObject(out, "finished", index_ >= total);
    }
    char* printed = cJSON_PrintUnformatted(out);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(out);
    cJSON_Delete(root);
    return result;
}

void SpeakLesson::InheritDailyAiIfNeeded() {
    LoadNvs();
    bool dirty = false;
    if (dai_ != "xiaozhi" && akey_.empty() && !dkey_.empty()) {
        akey_ = dkey_;
        if (amod_.empty()) {
            amod_ = dmod_;
        }
        if (avoi_.empty()) {
            avoi_ = dvoi_;
        }
        dirty = true;
        ESP_LOGI(TAG, "Teach Gemini key inherited from daily chat");
    }
    if (dai_ == "xiaozhi" && mac_.empty() && ddai_ != "xiaozhi" && !dkey_.empty()) {
        dai_ = ddai_;
        akey_ = dkey_;
        if (amod_.empty()) {
            amod_ = dmod_;
        }
        if (avoi_.empty()) {
            avoi_ = dvoi_;
        }
        dirty = true;
        ESP_LOGI(TAG, "Teach uses daily Gemini (no Xiaozhi MAC for lessons)");
    }
    if (dirty) {
        SaveNvs();
    }
}

std::string SpeakLesson::Start(const std::string& level, const std::string& unit, int index,
                               const std::string& backend, bool force_reconnect) {
    LoadNvs();
    if (!backend.empty()) {
        dai_ = NormalizeTeachBackend(backend);
    }
    InheritDailyAiIfNeeded();
    std::string ready = IdentityReadyError();
    if (!ready.empty()) {
        return std::string("{\"success\":false,\"error\":\"") + ready + "\"}";
    }
    if (!level.empty()) {
        level_ = level;
        for (char& c : level_) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
    }
    if (!unit.empty()) {
        unit_ = unit;
    }
    const std::string want_key = level_ + "/" + unit_;
    if (cached_key_ != want_key) {
        cached_json_.clear();
        cached_key_.clear();
    }
    const bool live = InLiveVoiceSession();
    if (live) {
        // Gemini MCP runs on the audio thread. Never block it on LAN JSON — intro
        // does not need the unit file; the next card/show will fetch.
        ESP_LOGI(TAG, "Start during live voice: intro first, JSON later");
    } else if (!EnsureUnitLoaded()) {
        return "{\"success\":false,\"error\":\"Khong tai JSON tu may (localhost:8765).\"}";
    }
    int total = 0;
    if (!cached_json_.empty()) {
        cJSON* root = cJSON_Parse(cached_json_.c_str());
        total = CardCount(root);
        cJSON_Delete(root);
    }
    const bool was_on = on_;
    if (index >= 0) {
        index_ = index;
    } else if (done_ask_ || (!was_on && total > 0 && index_ >= total)) {
        // Finished unit: học lại starts at card 0 of this topic.
        index_ = 0;
    }
    if (index_ < 0) {
        index_ = 0;
    }
    if (total > 0 && index_ > total) {
        index_ = total;
    }
    const uint8_t prev_slot = new_slot_;
    const bool was_new = style_new_;
    on_ = true;
    style_new_ = true;
    new_slot_ = 0;
    teach_armed_ = true;
    done_ask_ = false;
    EnsureClientId();
    SaveNvs();
    const bool need_switch = !was_new || prev_slot != 0;
    const auto want = DirectAiConfig::LoadBackend();
    const auto have = DirectAiConfig::RunningBackend();
    // UI :8080 always reconnects so the robot greets as teacher (same as Giao tiếp).
    // MCP during a live session must not kill the audio thread; apply identity on idle.
    if (force_reconnect || !live) {
        if (force_reconnect || need_switch || want != have) {
            ESP_LOGI(TAG, "Start: switch to teaching identity and greet");
            SwitchIdentity();
        }
    } else if (need_switch || want != have) {
        identity_pending_ = true;
        ESP_LOGI(TAG, "Start during live voice: identity after this turn");
    }
    intro_pending_ = true;
    intro_hold_ = false;
    intro_ready_ = false;
    if (!live) {
        ShowCurrent();
    }
    return CurrentCardJson();
}

std::string SpeakLesson::Stop() {
    LoadNvs();
    if (style_new_) {
        if (new_slot_ == 1 && !on_) {
            return StatusJson();
        }
        return GoDaily();
    }
    on_ = false;
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    SaveNvs();
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetPreviewImage(nullptr);
    }
    return StatusJson();
}

std::string SpeakLesson::ResetUnit() {
    LoadNvs();
    index_ = 0;
    SaveNvs();
    cached_json_.clear();
    return StatusJson();
}

std::string SpeakLesson::Next() {
    LoadNvs();
    if (on_ && done_ask_) {
        return DoneJson();
    }
    if (on_ && (intro_pending_ || (intro_hold_ && !intro_ready_))) {
        intro_pending_ = false;
        intro_hold_ = true;
        intro_ready_ = false;
        return IntroJson();
    }
    if (on_ && intro_hold_ && intro_ready_) {
        intro_hold_ = false;
        intro_ready_ = false;
        intro_pending_ = false;
        ShowCurrent();
        return CardJsonAt(index_, true);
    }
    if (!EnsureUnitLoaded()) {
        return "{\"error\":\"Khong tai JSON\"}";
    }
    cJSON* root = cJSON_Parse(cached_json_.c_str());
    int total = CardCount(root);
    cJSON_Delete(root);
    if (index_ < total) {
        index_++;
    }
    on_ = true;
    if (index_ >= total) {
        done_ask_ = true;
        teach_armed_ = true;
        SaveNvs();
        ESP_LOGI(TAG, "Unit done: ask restart %s/%s", level_.c_str(), unit_.c_str());
        return DoneJson();
    }
    SaveNvs();
    ShowCurrent();
    return CurrentCardJson();
}

std::string SpeakLesson::CurrentCardJson() {
    LoadNvs();
    if (on_ && done_ask_) {
        return DoneJson();
    }
    if (on_ && intro_pending_) {
        intro_pending_ = false;
        intro_hold_ = true;
        intro_ready_ = false;
        return IntroJson();
    }
    if (on_ && intro_hold_ && !intro_ready_) {
        return IntroJson();
    }
    intro_hold_ = false;
    intro_ready_ = false;
    return CardJsonAt(index_, true);
}

std::string SpeakLesson::StatusJson() {
    LoadNvs();
    int total = 0;
    int words_done = 0;
    int phrases_done = 0;
    std::string title_en;
    std::string title_vi;
    std::string goal;
    if (!cached_json_.empty()) {
        cJSON* root = cJSON_Parse(cached_json_.c_str());
        if (root) {
            total = CardCount(root);
            auto s = [&](const char* k) {
                cJSON* it = cJSON_GetObjectItem(root, k);
                return cJSON_IsString(it) ? it->valuestring : "";
            };
            title_en = s("title_en");
            title_vi = s("title_vi");
            goal = s("goal_vi");
            for (int i = 0; i < total && i < index_; ++i) {
                cJSON* card = CardAt(root, i);
                cJSON* type = card ? cJSON_GetObjectItem(card, "type") : nullptr;
                if (cJSON_IsString(type) && strcmp(type->valuestring, "phrase") == 0) {
                    phrases_done++;
                } else {
                    words_done++;
                }
            }
            cJSON_Delete(root);
        }
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "on", on_);
    cJSON_AddStringToObject(out, "style", style_new_ ? "new" : "old");
    cJSON_AddStringToObject(out, "slot", SlotName(new_slot_));
    std::string active_mac = ActiveDeviceMac();
    cJSON_AddStringToObject(out, "active_mac", active_mac.c_str());
    std::string active_id = ActiveClientId();
    cJSON_AddStringToObject(out, "active_client_id", active_id.c_str());
    cJSON_AddStringToObject(out, "daily_mac", dmac_.c_str());
    cJSON_AddStringToObject(out, "daily_client_id", duid_.c_str());
    cJSON_AddStringToObject(out, "new_custom_mac", cmac_.c_str());
    cJSON_AddStringToObject(out, "new_custom_client_id", cuid_.c_str());
    const bool need_intro = on_ && (intro_pending_ || (intro_hold_ && !intro_ready_));
    const bool done = on_ && done_ask_;
    cJSON_AddBoolToObject(out, "need_intro", need_intro);
    cJSON_AddBoolToObject(out, "finished", done || (total > 0 && index_ >= total));
    cJSON_AddStringToObject(out, "phase", done ? "done" : (need_intro ? "intro" : "card"));
    if (done) {
        std::string nxt = NextUnitId(unit_);
        const char* say = nxt.empty() ? kDoneSayLast : kDoneSay;
        cJSON_AddStringToObject(out, "say", say);
        if (!nxt.empty()) {
            cJSON_AddStringToObject(out, "next_unit", nxt.c_str());
        }
    } else if (need_intro) {
        cJSON_AddStringToObject(out, "say", kIntroSay);
    }
    cJSON_AddStringToObject(out, "base", base_.c_str());
    cJSON_AddStringToObject(out, "level", level_.c_str());
    cJSON_AddStringToObject(out, "unit", unit_.c_str());
    cJSON_AddNumberToObject(out, "index", index_);
    cJSON_AddNumberToObject(out, "total", total);
    cJSON_AddNumberToObject(out, "words_done", words_done);
    cJSON_AddNumberToObject(out, "phrases_done", phrases_done);
    cJSON_AddStringToObject(out, "title_en", title_en.c_str());
    cJSON_AddStringToObject(out, "title_vi", title_vi.c_str());
    cJSON_AddStringToObject(out, "goal_vi", goal.c_str());
    cJSON_AddStringToObject(out, "student_name", WebSocketControlServer::GetStudentName().c_str());
    cJSON_AddNumberToObject(out, "age", age_);
    cJSON_AddStringToObject(out, "dob", dob_.c_str());
    cJSON_AddStringToObject(out, "likes", likes_.c_str());
    cJSON_AddStringToObject(out, "mac", mac_.c_str());
    cJSON_AddStringToObject(out, "client_id", uuid_.c_str());
    cJSON_AddStringToObject(out, "ai_backend", dai_.c_str());
    cJSON_AddBoolToObject(out, "has_key", !akey_.empty());
    std::string key_hint = MaskTeachKey(akey_);
    cJSON_AddStringToObject(out, "key_hint", key_hint.c_str());
    cJSON_AddStringToObject(out, "model", amod_.c_str());
    cJSON_AddStringToObject(out, "voice", avoi_.c_str());
    cJSON_AddStringToObject(out, "instructions_speak", ains_.c_str());
    cJSON_AddStringToObject(out, "daily_ai_backend", ddai_.c_str());
    cJSON_AddBoolToObject(out, "daily_has_key", !dkey_.empty());
    std::string daily_key_hint = MaskTeachKey(dkey_);
    cJSON_AddStringToObject(out, "daily_key_hint", daily_key_hint.c_str());
    cJSON_AddStringToObject(out, "daily_model", dmod_.c_str());
    cJSON_AddStringToObject(out, "daily_voice", dvoi_.c_str());
    std::string prompt = XiaozhiPrompt();
    cJSON_AddStringToObject(out, "xiaozhi_prompt", prompt.c_str());
    std::string prompt_en = XiaozhiPromptEn();
    cJSON_AddStringToObject(out, "xiaozhi_prompt_en", prompt_en.c_str());
    cJSON_AddBoolToObject(out, "sub_robot", sub_robot_);
    cJSON_AddBoolToObject(out, "sub_stt", sub_stt_);
    cJSON_AddStringToObject(out, "explain_level",
                            explain_level_ >= 2 ? "detailed" : (explain_level_ == 1 ? "basic" : "off"));
    char* printed = cJSON_PrintUnformatted(out);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(out);
    return result;
}

std::string SpeakLesson::UnitJson() {
    if (!EnsureUnitLoaded()) {
        return "{\"error\":\"Khong tai JSON tu may (localhost:8765).\"}";
    }
    return cached_json_;
}

bool SpeakLesson::ShowImage(const std::string& url) {
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (!display) {
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return false;
    }
    auto http = network->CreateHttp(4);
    if (!http) {
        ESP_LOGW(TAG, "ShowImage CreateHttp failed: %s", url.c_str());
        return false;
    }
    http->SetTimeout(5000);
    http->SetKeepAlive(false);
    http->SetHeader("Connection", "close");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "ShowImage open failed: %s", url.c_str());
        return false;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "ShowImage HTTP %d: %s", http->GetStatusCode(), url.c_str());
        http->Close();
        return false;
    }

    auto alloc_jpeg = [](size_t n) -> char* {
        char* p = static_cast<char*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (p == nullptr) {
            p = static_cast<char*>(heap_caps_malloc(n, MALLOC_CAP_8BIT));
        }
        return p;
    };

    char* data = nullptr;
    size_t jpeg_len = 0;
    size_t content_length = http->GetBodyLength();
    auto read_loop = [&](char* dest, size_t max_n) -> size_t {
        size_t total_read = 0;
        while (total_read < max_n) {
            int ret = http->Read(dest + total_read, max_n - total_read);
            if (ret < 0) {
                return 0;
            }
            if (ret == 0) {
                break;
            }
            total_read += static_cast<size_t>(ret);
        }
        return total_read;
    };
    if (content_length == 0 || content_length > 80 * 1024) {
        // No Content-Length: drain with Read() (ReadAll deadlocks above 8KB).
        std::string body;
        char tmp[512];
        while (body.size() < 80 * 1024) {
            int ret = http->Read(tmp, sizeof(tmp));
            if (ret < 0) {
                http->Close();
                return false;
            }
            if (ret == 0) {
                break;
            }
            body.append(tmp, static_cast<size_t>(ret));
        }
        http->Close();
        if (body.empty() || body.size() > 80 * 1024) {
            return false;
        }
        data = alloc_jpeg(body.size());
        if (!data) {
            return false;
        }
        memcpy(data, body.data(), body.size());
        jpeg_len = body.size();
    } else {
        data = alloc_jpeg(content_length);
        if (!data) {
            http->Close();
            return false;
        }
        jpeg_len = read_loop(data, content_length);
        http->Close();
        if (jpeg_len == 0) {
            heap_caps_free(data);
            return false;
        }
    }

    try {
        display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(data, jpeg_len));
        ESP_LOGI(TAG, "ShowImage ok %s (%u bytes)", url.c_str(), (unsigned)jpeg_len);
        return true;
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "ShowImage decode failed %s: %s", url.c_str(), e.what());
        return false;
    }
}

std::string SpeakLesson::ShowCurrent() {
    LoadNvs();
    if (on_ && done_ask_) {
        return DoneJson();
    }
    if (on_ && (intro_pending_ || (intro_hold_ && !intro_ready_))) {
        std::string intro = IntroJson();
        cJSON* root = cJSON_Parse(intro.c_str());
        if (root) {
            cJSON_AddBoolToObject(root, "shown", false);
            char* printed = cJSON_PrintUnformatted(root);
            intro = printed ? printed : intro;
            cJSON_free(printed);
            cJSON_Delete(root);
        }
        return intro;
    }
    std::string card = CardJsonAt(index_, true);
    cJSON* root = cJSON_Parse(card.c_str());
    if (!root) {
        return card;
    }
    cJSON* image = cJSON_GetObjectItem(root, "image");
    bool ok = false;
    if (cJSON_IsString(image)) {
        ok = ShowImage(image->valuestring);
    }
    cJSON_AddBoolToObject(root, "shown", ok);
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed ? printed : card;
    cJSON_free(printed);
    cJSON_Delete(root);
    return result;
}

std::string SpeakLesson::IntroJson() const {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "on", true);
    cJSON_AddStringToObject(out, "phase", "intro");
    cJSON_AddBoolToObject(out, "need_intro", true);
    cJSON_AddBoolToObject(out, "skip_image", true);
    cJSON_AddStringToObject(out, "say", kIntroSay);
    cJSON_AddStringToObject(out, "teacher", kIntroSay);
    cJSON_AddStringToObject(out, "level", level_.c_str());
    cJSON_AddStringToObject(out, "unit", unit_.c_str());
    cJSON_AddNumberToObject(out, "index", index_);
    char* printed = cJSON_PrintUnformatted(out);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(out);
    return result;
}

std::string SpeakLesson::DoneJson() const {
    std::string nxt = NextUnitId(unit_);
    const char* say = nxt.empty() ? kDoneSayLast : kDoneSay;
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "on", true);
    cJSON_AddBoolToObject(out, "finished", true);
    cJSON_AddStringToObject(out, "phase", "done");
    cJSON_AddBoolToObject(out, "need_intro", false);
    cJSON_AddBoolToObject(out, "skip_image", true);
    cJSON_AddStringToObject(out, "say", say);
    cJSON_AddStringToObject(out, "teacher", say);
    cJSON_AddStringToObject(out, "level", level_.c_str());
    cJSON_AddStringToObject(out, "unit", unit_.c_str());
    cJSON_AddNumberToObject(out, "index", index_);
    if (!nxt.empty()) {
        cJSON_AddStringToObject(out, "next_unit", nxt.c_str());
    }
    cJSON_AddStringToObject(out, "message",
                            nxt.empty()
                                ? "Het chu de cuoi. Noi field say. Hoc lai: start index=0. Nghi: self.speak.off."
                                : "Het chu de. Noi field say. Lai: start index=0. Tiep: start index=0 unit=next_unit. Nghi: self.speak.off.");
    char* printed = cJSON_PrintUnformatted(out);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(out);
    return result;
}

void SpeakLesson::Disarm() {
    LoadNvs();
    if (!on_ && !style_new_) {
        return;
    }
    on_ = false;
    style_new_ = false;
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    identity_pending_ = false;
    done_ask_ = false;
    SaveNvs();
    ESP_LOGI(TAG, "Disarm: old English (World Edu / Daily) Device-Id takes over");
}

void SpeakLesson::OnNewTalkSession() {
    LoadNvs();
    if (!on_) {
        return;
    }
    if (done_ask_) {
        ESP_LOGI(TAG, "New talk session: still asking restart");
        return;
    }
    intro_pending_ = true;
    intro_hold_ = false;
    intro_ready_ = false;
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetPreviewImage(nullptr);
    }
    ESP_LOGI(TAG, "New talk session: intro before first card");
}

void SpeakLesson::OnAssistantTurnDone() {
    if (intro_hold_ && !intro_ready_) {
        intro_ready_ = true;
        ESP_LOGI(TAG, "Intro spoken, next card may start the unit");
    }
}

void SpeakLesson::OnTalkIdle() {
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetPreviewImage(nullptr);
    }
    ESP_LOGI(TAG, "Talk idle: restored default eyes");
    if (identity_pending_ && on_) {
        identity_pending_ = false;
        teach_armed_ = true;
        ESP_LOGI(TAG, "Talk idle: apply teaching identity and greet");
        SwitchIdentity();
        return;
    }
    if (done_ask_ && on_) {
        ESP_LOGI(TAG, "Talk idle: keep lesson to hear restart answer");
        return;
    }
    // Next Hi Jason must be daily chat, not leftover teach NVS.
    if (style_new_ && (on_ || new_slot_ == 0 || teach_armed_)) {
        ParkLessonToDaily();
    }
}

std::string SpeakLesson::ProfileLine() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    std::string name = WebSocketControlServer::GetStudentName();
    if (name.empty()) {
        name = "bạn nhỏ";
    }
    std::string line = "Học viên: " + name;
    if (self.age_ > 0) {
        line += ", tuổi " + std::to_string(self.age_);
    }
    if (!self.dob_.empty()) {
        line += ", sinh " + self.dob_;
    }
    if (!self.likes_.empty()) {
        line += ", thích " + self.likes_;
    }
    line += ". Unit " + self.level_ + "/" + self.unit_ + ".";
    return line;
}

std::string SpeakLesson::ProfileLineEn() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    std::string name = WebSocketControlServer::GetStudentName();
    if (name.empty()) {
        name = "the child";
    }
    std::string line = "Student: " + name;
    if (self.age_ > 0) {
        line += ", age " + std::to_string(self.age_);
    }
    if (!self.dob_.empty()) {
        line += ", born " + self.dob_;
    }
    if (!self.likes_.empty()) {
        line += ", likes " + self.likes_;
    }
    line += ". Unit " + self.level_ + "/" + self.unit_ + ".";
    return line;
}

std::string SpeakLesson::XiaozhiPrompt() const {
    return std::string(kXiaozhiPrompt) + "\n" + ProfileLine();
}

std::string SpeakLesson::XiaozhiPromptEn() const {
    return std::string(kXiaozhiPromptEn) + "\n" + ProfileLineEn();
}

std::string SpeakLesson::DirectAiExtraPrompt() const {
    if (!IsOn()) {
        return "";
    }
    return std::string(kDirectPrompt) + "\n" + ProfileLine();
}

void SpeakLesson::SetProfile(int age, const std::string& likes, const std::string& dob) {
    LoadNvs();
    if (age >= 0 && age < 18) {
        age_ = age;
    }
    if (dob.size() <= 10) {
        dob_ = dob;
    }
    likes_ = likes.substr(0, 180);
    SaveNvs();
}

std::string SpeakLesson::ProfileJson() const {
    return const_cast<SpeakLesson*>(this)->StatusJson();
}

std::string OttoSpeakDirectAiPrompt() {
    return SpeakLesson::Get().DirectAiExtraPrompt();
}

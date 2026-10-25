#include "speak_lesson.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <esp_random.h>

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "device_state.h"
#include "otto_emoji_display.h"
#include <http.h>
#include "lvgl_display.h"
#include "lvgl_image.h"

#include <stdexcept>
#include "direct_ai_config.h"
#include "device_identity_presets.h"
#include "settings.h"
#include "speak_progress.h"
#include "websocket_control_server.h"

#define TAG "SpeakLesson"
#define SPEAK_NVS "speak"

namespace {

constexpr const char* kIntroSay =
    "Mình có nhiệm vụ dạy tiếng Anh cho bạn đây, mình bắt đầu học nhé!";

constexpr const char* kDoneSay =
    "Bạn học xong bài này rồi. Bạn muốn học bài tiếp theo hay nghỉ nói chuyện?";

constexpr const char* kDoneSayLast =
    "Bạn học xong chủ đề cuối rồi. Bạn muốn học lại từ đầu hay nghỉ nói chuyện?";

constexpr const char* kCourseLoadingVi = "Đang tải khóa học";

void ShowCourseLoadingFace() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    display->SetStatus(kCourseLoadingVi);
    if (auto* otto = dynamic_cast<OttoEmojiDisplay*>(display)) {
        otto->ShowCourseLoading(kCourseLoadingVi);
    } else {
        display->SetChatMessage("system", kCourseLoadingVi);
    }
}

void HideCourseLoadingFace() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    if (auto* otto = dynamic_cast<OttoEmojiDisplay*>(display)) {
        otto->HideCourseLoading();
    }
}

struct PauseAfeForHttp {
    PauseAfeForHttp() : audio_(Application::GetInstance().GetAudioService()) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        audio_.EnableVoiceProcessing(false);
        audio_.EnableWakeWordDetection(false);
    }
    ~PauseAfeForHttp() {
        if (SpeakLesson::Get().HoldMicForIntro()) {
            return;
        }
        const DeviceState st = Application::GetInstance().GetDeviceState();
        if (st == kDeviceStateListening) {
            audio_.EnableVoiceProcessing(true);
        } else if (st == kDeviceStateIdle || st == kDeviceStateStarting ||
                   st == kDeviceStateActivating) {
            audio_.EnableWakeWordDetection(true);
        }
    }
    AudioService& audio_;
};

struct CourseLoadGuard {
    explicit CourseLoadGuard(bool on) : on_(on) {
        if (on_) {
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            ShowCourseLoadingFace();
        }
    }
    ~CourseLoadGuard() {
        if (on_ && hide_on_exit_) {
            HideCourseLoadingFace();
        }
    }
    void KeepVisible() { hide_on_exit_ = false; }
    CourseLoadGuard(const CourseLoadGuard&) = delete;
    CourseLoadGuard& operator=(const CourseLoadGuard&) = delete;

    bool on_;
    bool hide_on_exit_ = true;
};

constexpr const char* kXiaozhiPrompt =
    "CẤM STT YouTube: server hay trả ĐÚNG nguyên câu, không phải bé nói. "
    "Nghe 1 câu này thì im, CẤM khen / tạm biệt / subscribe, nhắc từ đang học:\n"
    "1) Cảm ơn các bạn đã theo dõi và hẹn gặp lại\n"
    "2) Hãy subscribe cho kênh La La School để không bỏ lỡ những video hấp dẫn\n"
    "3) Hãy đăng ký kênh để ủng hộ kênh của mình nhé!\n"
    "4) Hãy subscribe cho kênh Ghiền Mì Gõ Để không bỏ lỡ\n"
    "5) Bye\n"
    "Khi on=true bạn chỉ NÓI. Robot Self-Control đã kéo JSON + ảnh. "
    "CẤM status/show/heard/next/start. card tối đa 1 lần lúc học ngay. Gọi tool lặp = lỗi 400 tool_call_id.\n"
    "Cue robot:\n"
    "- học ngay / thẻ mới: đọc speech[]/teacher, mời bạn nói, DỪNG. Ảnh đã hiện.\n"
    "- hãy khơi gợi: nhắc từ đang học. hết bài: CHỈ nói say.\n"
    "Khi BẠN nghe bé nói: đúng/gần đúng thì khen ngắn. Sai: CHƯA phải, đọc mẫu từ đang hiện 2 lần, "
    "mời nói lại, DỪNG. CẤM khen đúng khi sai. CẤM card/show/heard/next. "
    "CẤM bịa 'phát âm chuẩn'.\n"
    "CẤM nói giao tiếp hằng ngày / chưa mở bài / Mình chuyển sang học nhé.\n"
    "hide_answer=true: đừng đọc đáp án. phase=done: CHỈ nói say.\n"
    "Khi on=false: trò chuyện đời thường.";

constexpr const char* kXiaozhiPromptEn =
    "BAN YouTube STT: server often returns these EXACT lines — not the child. Stay silent, do not praise/goodbye/subscribe:\n"
    "1) Cảm ơn các bạn đã theo dõi và hẹn gặp lại\n"
    "2) Hãy subscribe cho kênh La La School để không bỏ lỡ những video hấp dẫn\n"
    "3) Hãy đăng ký kênh để ủng hộ kênh của mình nhé!\n"
    "4) Hãy subscribe cho kênh Ghiền Mì Gõ Để không bỏ lỡ\n"
    "5) Bye\n"
    "When on=true only SPEAK. Robot already loaded JSON and the image. "
    "Do NOT call heard/next/status/show/start. card at most ONCE on học ngay.\n"
    "học ngay / thẻ mới: say speech[]/teacher, then wait.\n"
    "When you HEAR the child speak (same as daily chat): short praise or nudge. No tools.\n"
    "hãy khơi gợi: nudge the current word. hết bài: say field say only.\n"
    "Do not claim perfect pronunciation.\n"
    "When on=false: chat normally.";

constexpr const char* kXiaozhiDailyPrompt =
    "CẤM STT YouTube: server hay trả ĐÚNG nguyên câu, không phải bé nói. "
    "Nghe 1 câu này thì im, CẤM khen / tạm biệt / subscribe:\n"
    "1) Cảm ơn các bạn đã theo dõi và hẹn gặp lại\n"
    "2) Hãy subscribe cho kênh La La School để không bỏ lỡ những video hấp dẫn\n"
    "3) Hãy đăng ký kênh để ủng hộ kênh của mình nhé!\n"
    "4) Hãy subscribe cho kênh Ghiền Mì Gõ Để không bỏ lỡ\n"
    "5) Bye\n"
    "Bạn là Emun. Xưng mình, gọi bé là bạn. Không tự xưng robot. Không gọi bạn là con.\n"
    "Nói chuyện hằng ngày: vui, ngắn, dễ hiểu.\n"
    "Nghe 'hãy khơi gợi' hoặc 'khơi gợi lại': tiếp tục trò chuyện / nhắc nhẹ, không hỏi dài, "
    "CẤM gọi status nhiều lần.\n"
    "Chỉ gọi self.speak.status MỘT LẦN khi bạn muốn học hoặc không chắc đang ở chế độ nào.\n"
    "\n"
    "A) on=false — GIAO TIẾP HẰNG NGÀY\n"
    "Trò chuyện đời thường. Không dạy thẻ, không gọi card/next/show.\n"
    "Bạn nói học / học tiếng Anh / học unit / đổi chủ đề: gọi self.speak.start "
    "level=a1 unit=u01–u43 (unit bạn chọn) index=0, ĐỂ TRỐNG backend "
    "(dùng AI dạy học đã lưu trên :8080).\n"
    "Nói đúng một câu: \"Mình chuyển sang học nhé!\" rồi DỪNG. "
    "Phiên giao tiếp đóng và mở AI dạy. Đừng giảng bài trên phiên này.\n"
    "\n"
    "B) on=true hoặc nghe 'học ngay' — ĐANG HỌC.\n"
    "Robot Self-Control đã kéo JSON + ảnh. CẤM heard/next/show/status/start.\n"
    "học ngay / thẻ mới: card MỘT LẦN nếu cần, đọc teacher, chờ bạn nói.\n"
    "Nghe bé nói: đúng thì khen. Sai: đọc mẫu từ đang hiện, mời nói lại. CẤM tool/card.\n"
    "hết bài: CHỈ nói say. Không tuyên bố phát âm hoàn hảo.\n"
    "CẤM đọc đáp án khi hide_answer=true.\n"
    "Học lại: start index=0 cùng unit. Học tiếp: firmware tự tải bài Emun kế. "
    "CẤM self.otto.next_unit (World Edu). Nghỉ: self.speak.off hoặc self.speak.daily.\n"
    "Không chữa phát âm. Không bịa từ ngoài thẻ.";

bool ContainsWord(const std::string& hay, const std::string& needle) {
    if (needle.empty() || hay.size() < needle.size()) {
        return false;
    }
    std::string h = hay;
    std::string n = needle;
    for (char& c : h) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    for (char& c : n) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    size_t pos = 0;
    while ((pos = h.find(n, pos)) != std::string::npos) {
        const bool left = (pos == 0) || !std::isalnum(static_cast<unsigned char>(h[pos - 1]));
        const size_t end = pos + n.size();
        const bool right = (end >= h.size()) || !std::isalnum(static_cast<unsigned char>(h[end]));
        if (left && right) {
            return true;
        }
        pos = end;
    }
    return false;
}

std::string FoldEnglish(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            space = false;
        } else if (!out.empty() && !space) {
            out.push_back(' ');
            space = true;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

bool ContainsEnglish(const std::string& hay, const std::string& needle) {
    const std::string n = FoldEnglish(needle);
    if (n.empty()) {
        return false;
    }
    return ContainsWord(FoldEnglish(hay), n);
}

bool IsFirmwareCue(const std::string& text) {
    return text == "bắt đầu học ngay" || text == "sang thẻ tiếp" || text == "nói feedback" ||
           text == "hết bài" || text == "nhắc học viên nói" || text == "khơi gợi lại" ||
           text == "hãy khơi gợi" || text == "thẻ mới" || text == "nói khen" ||
           text == "khen rồi thẻ mới" || text == "chưa khớp" ||
           text == "học ngay" || text == "Hi,Jason" || text == "Hi Jason" ||
           text == "xin chào" || text == "Hi,ESP" || text == "Hi ESP";
}

bool IsLessonFiller(const std::string& text) {
    return ContainsWord(text, "cảm ơn") || ContainsWord(text, "thanks") ||
           ContainsWord(text, "thank you") || ContainsWord(text, "okay") ||
           ContainsWord(text, "ok") || ContainsWord(text, "oke") ||
           ContainsWord(text, "ừ") || ContainsWord(text, "rồi") ||
           ContainsWord(text, "yeah") || ContainsWord(text, "không có chi");
}

std::string FoldLoose(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool space = true;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c + 32);
        }
        if (c == '!' || c == '?' || c == '.' || c == ',' || c == ';' || c == ':' ||
            c == '"' || c == '\'' || c == '(' || c == ')') {
            c = ' ';
        }
        if (c <= ' ' ) {
            if (!space && !out.empty()) {
                out.push_back(' ');
                space = true;
            }
            continue;
        }
        out.push_back(static_cast<char>(c));
        space = false;
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

bool IsShortBye(const std::string& folded) {
    return folded == "bye" || folded == "bye bye" || folded == "good bye" ||
           folded == "goodbye";
}

bool IsSttYoutubeGhost(const std::string& text) {
    const std::string f = FoldLoose(text);
    if (f.empty()) {
        return false;
    }
    if (IsShortBye(f)) {
        return true;
    }
    static const char* kExact[] = {
        "cảm ơn các bạn đã theo dõi và hẹn gặp lại",
        "hãy subscribe cho kênh la la school để không bỏ lỡ những video hấp dẫn",
        "hãy đăng ký kênh để ủng hộ kênh của mình nhé",
        "hãy subscribe cho kênh ghiền mì gõ để không bỏ lỡ",
    };
    for (const char* p : kExact) {
        if (f == FoldLoose(p)) {
            return true;
        }
    }
    return false;
}

constexpr const char* kDirectPrompt =
    "CHẾ ĐỘ HỌC nói A1. Xưng mình, gọi bạn. Robot đã kéo JSON + ảnh. CẤM mọi self.speak.* "
    "(card/show/heard/next/status/start). Firmware tự chấm STT và hiện ảnh.\n"
    "Cue có nguyên văn trong ngoặc kép: đọc HẾT một hơi, đúng từ thẻ, CẤM bịa từ khác, mời nói, DỪNG.\n"
    "Khi BẠN nghe bé nói (như chat thường): khen / khích lệ ngắn ngay. CẤM tool. "
    "CẤM self.conversation.end. Cảm ơn / tạm biệt lúc học không phải hết bài, đừng tạm biệt. "
    "CẤM bịa phát âm chuẩn. Không tuyên bố phát âm hoàn hảo.\n"
    "chưa khớp: CẤM nói đúng rồi / phát âm chuẩn. Đọc mẫu từ đang hiện, mời nói lại. "
    "hãy khơi gợi / nhắc học viên nói: nhắc từ đang học. "
    "hết bài: đọc say, hỏi học tiếp hay nghỉ. CẤM self.otto.next_unit (đó là World Edu).\n"
    "CẤM đọc answer_en khi hide_answer=true. hide_answer=true: đừng đọc đáp án.\n";

std::string NextUnitId(const std::string& unit) {
    int n = 0;
    if (unit.size() >= 2 && (unit[0] == 'u' || unit[0] == 'U')) {
        n = atoi(unit.c_str() + 1);
    }
    if (n < 1 || n >= 43) {
        return "";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "u%02d", n + 1);
    return std::string(buf);
}

constexpr const char* kDefaultLan = "http://192.168.100.161:8765";
constexpr const char* kDefaultCloud = "https://emun.pages.dev";

std::string TrimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

bool LooksHttps(const std::string& url) {
    return url.size() >= 8 && url.compare(0, 8, "https://") == 0;
}

uint8_t ParseCourseSource(const std::string& s) {
    if (s == "cloud" || s == "pages" || s == "https") {
        return 2;
    }
    if (s == "lan" || s == "local" || s == "http") {
        return 1;
    }
    return 0;
}

const char* CourseSourceName(uint8_t src) {
    if (src == 2) {
        return "cloud";
    }
    if (src == 1) {
        return "lan";
    }
    return "auto";
}

bool AllowedTeachSttCode(const std::string& code) {
    return code == "en-US" || code == "en-GB" || code == "en-IN" || code == "en";
}

std::vector<std::string> SplitTeachSttCodes(const std::string& csv) {
    std::vector<std::string> out;
    std::string tok;
    auto flush = [&]() {
        if (!AllowedTeachSttCode(tok)) {
            tok.clear();
            return;
        }
        for (const auto& seen : out) {
            if (seen == tok) {
                tok.clear();
                return;
            }
        }
        out.push_back(tok);
        tok.clear();
    };
    for (unsigned char c : csv) {
        if (c == ',' || c == ';' || c == ' ' || c == '|') {
            flush();
        } else {
            tok.push_back(static_cast<char>(c));
        }
    }
    flush();
    if (out.empty()) {
        out.push_back("en-US");
    }
    return out;
}

std::string SanitizeTeachSttCodes(const std::string& raw) {
    std::string out;
    for (const auto& code : SplitTeachSttCodes(raw)) {
        if (!out.empty()) {
            out += ',';
        }
        out += code;
    }
    return out;
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

cJSON* FindCardById(cJSON* root, const char* id) {
    if (!root || !id || !id[0]) {
        return nullptr;
    }
    cJSON* cards = cJSON_GetObjectItem(root, "cards");
    if (!cJSON_IsArray(cards)) {
        return nullptr;
    }
    const int n = cJSON_GetArraySize(cards);
    for (int i = 0; i < n; ++i) {
        cJSON* card = cJSON_GetArrayItem(cards, i);
        cJSON* cid = card ? cJSON_GetObjectItem(card, "id") : nullptr;
        if (cJSON_IsString(cid) && strcmp(cid->valuestring, id) == 0) {
            return card;
        }
    }
    return nullptr;
}

cJSON* FindActivity(cJSON* root, const std::string& id) {
    cJSON* acts = cJSON_GetObjectItem(root, "activities");
    if (!cJSON_IsArray(acts) || id.empty()) {
        return nullptr;
    }
    const int n = cJSON_GetArraySize(acts);
    cJSON* legacy_hit = nullptr;
    for (int i = 0; i < n; ++i) {
        cJSON* act = cJSON_GetArrayItem(acts, i);
        cJSON* aid = act ? cJSON_GetObjectItem(act, "id") : nullptr;
        if (cJSON_IsString(aid) && id == aid->valuestring) {
            return act;
        }
        cJSON* legacy = act ? cJSON_GetObjectItem(act, "legacy_ids") : nullptr;
        if (cJSON_IsArray(legacy)) {
            const int ln = cJSON_GetArraySize(legacy);
            for (int j = 0; j < ln; ++j) {
                cJSON* it = cJSON_GetArrayItem(legacy, j);
                if (cJSON_IsString(it) && id == it->valuestring) {
                    legacy_hit = act;
                    break;
                }
            }
        }
        if (!legacy_hit && id == "flip-pairs" && cJSON_IsString(aid) &&
            strcmp(aid->valuestring, "flip-and-say") == 0) {
            legacy_hit = act;
        }
    }
    return legacy_hit;
}

const char* JsonStr(cJSON* obj, const char* key) {
    if (!obj) {
        return "";
    }
    cJSON* it = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(it) ? it->valuestring : "";
}

std::string QuoteAfter(const char* text, const char* key) {
    if (!text || !key || !key[0]) {
        return "";
    }
    const char* hit = strstr(text, key);
    if (!hit) {
        return "";
    }
    hit += strlen(key);
    const char* end = strchr(hit, '"');
    if (!end || end == hit || (end - hit) > 80) {
        return "";
    }
    return std::string(hit, end);
}

std::string ExtractAskQuestion(cJSON* card) {
    if (!card) {
        return "";
    }
    const char* direct = JsonStr(card, "question_en");
    if (direct && direct[0]) {
        return direct;
    }
    cJSON* flow = cJSON_GetObjectItem(card, "teaching_flow");
    const char* flow_q = JsonStr(flow, "question_en");
    if (flow_q && flow_q[0]) {
        return flow_q;
    }
    cJSON* explain = cJSON_GetObjectItem(card, "explain");
    const char* blocks[] = {"basic", "detailed"};
    for (const char* name : blocks) {
        cJSON* block = cJSON_IsObject(explain) ? cJSON_GetObjectItem(explain, name) : nullptr;
        const char* en = JsonStr(block, "en");
        std::string q = QuoteAfter(en, "Emun asks \"");
        if (q.empty()) {
            q = QuoteAfter(en, "asks \"");
        }
        if (q.empty()) {
            q = QuoteAfter(en, "question \"");
        }
        if (!q.empty()) {
            return q;
        }
    }
    return "";
}

std::string FirstActivityId(cJSON* root) {
    cJSON* acts = cJSON_GetObjectItem(root, "activities");
    if (!cJSON_IsArray(acts) || cJSON_GetArraySize(acts) <= 0) {
        return "";
    }
    return JsonStr(cJSON_GetArrayItem(acts, 0), "id");
}

std::string GamePathFromUrl(const char* url, const std::string& level, const std::string& unit,
                            const char* id) {
    if (url && *url) {
        const char* game = strstr(url, "/game/");
        if (game) {
            return std::string(game + 1);
        }
        const char* slash = strrchr(url, '/');
        if (slash && slash[1]) {
            return std::string("game/") + level + "/" + unit + "/" + (slash + 1);
        }
    }
    return std::string("game/") + level + "/" + unit + "/" + (id ? id : "card") + ".jpg";
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
    const std::string stored = TrimSlash(s.GetString("base", kDefaultLan));
    cloud_ = TrimSlash(s.GetString("cld", kDefaultCloud));
    if (cloud_.empty()) {
        cloud_ = kDefaultCloud;
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
    stt_english_ = s.GetBool("sten", true);
    stt_codes_ = SanitizeTeachSttCodes(s.GetString("stc", "en-US"));
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
    uint8_t src_saved = 255;
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
        uint8_t bsrc = 255;
        if (nvs_get_u8(nvs, "bsrc", &bsrc) == ESP_OK && bsrc <= 2) {
            src_saved = bsrc;
        }
        size_t alen = 0;
        if (nvs_get_str(nvs, "act", nullptr, &alen) == ESP_OK && alen > 1 && alen < 32) {
            activity_id_.assign(alen, '\0');
            nvs_get_str(nvs, "act", activity_id_.data(), &alen);
            if (!activity_id_.empty() && activity_id_.back() == '\0') {
                activity_id_.pop_back();
            }
        }
        nvs_close(nvs);
    }
    if (LooksHttps(stored)) {
        cloud_ = stored;
        base_ = kDefaultLan;
        src_ = (src_saved <= 2) ? src_saved : 2;
    } else {
        base_ = stored.empty() ? kDefaultLan : stored;
        src_ = (src_saved <= 2) ? src_saved : 0;
    }
    SpeakProgress::Get();
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
    nvs_set_str(nvs, "cld", cloud_.c_str());
    nvs_set_u8(nvs, "bsrc", src_);
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
    nvs_set_u8(nvs, "sten", stt_english_ ? 1 : 0);
    nvs_set_str(nvs, "stc", stt_codes_.c_str());
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
    nvs_set_str(nvs, "act", activity_id_.c_str());
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

bool SpeakLesson::IsTeaching() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    return self.on_ && !self.done_ask_;
}

bool SpeakLesson::IsDoneAsk() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (!self.on_) {
        return false;
    }
    if (self.done_ask_) {
        return true;
    }
    const int total = self.LessonTotal();
    return total > 0 && self.index_ >= total;
}

std::string SpeakLesson::DoneSay() const {
    return NextUnitId(unit_).empty() && NextActivityId().empty() ? kDoneSayLast : kDoneSay;
}

std::string SpeakLesson::NextActivityId() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (self.cached_act_json_.empty()) {
        return "";
    }
    cJSON* root = cJSON_Parse(self.cached_act_json_.c_str());
    if (!root) {
        return "";
    }
    cJSON* acts = cJSON_GetObjectItem(root, "activities");
    std::string found;
    if (cJSON_IsArray(acts)) {
        const int n = cJSON_GetArraySize(acts);
        int cur = -1;
        for (int i = 0; i < n; ++i) {
            if (self.activity_id_ == JsonStr(cJSON_GetArrayItem(acts, i), "id")) {
                cur = i;
                break;
            }
        }
        for (int i = cur + 1; i < n; ++i) {
            cJSON* act = cJSON_GetArrayItem(acts, i);
            const char* id = JsonStr(act, "id");
            const char* type = JsonStr(act, "type");
            if (!id || !*id) {
                continue;
            }
            if (type && strcmp(type, "unit_review") == 0 && !self.ReviewUnlocked()) {
                continue;
            }
            found = id;
            break;
        }
    }
    cJSON_Delete(root);
    return found;
}

std::string SpeakLesson::ContinueNextLesson() {
    LoadNvs();
    const std::string next_act = NextActivityId();
    if (!next_act.empty()) {
        ESP_LOGI(TAG, "Continue next activity %s/%s %s", level_.c_str(), unit_.c_str(),
                 next_act.c_str());
        return Start(level_, unit_, 0, "", true, next_act);
    }
    const std::string nxt = NextUnitId(unit_);
    if (nxt.empty()) {
        ESP_LOGI(TAG, "Continue: no next lesson after %s/%s", level_.c_str(), unit_.c_str());
        return DoneJson();
    }
    ESP_LOGI(TAG, "Continue next unit %s/%s", level_.c_str(), nxt.c_str());
    return Start(level_, nxt, 0, "", true, "");
}

bool SpeakLesson::UsesXiaozhiTeach() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    return self.on_ && !self.done_ask_ && self.dai_ == "xiaozhi";
}

const char* SpeakLesson::StartDetectCue() const {
    return UsesXiaozhiTeach() ? "học ngay" : "bắt đầu học ngay";
}

const char* SpeakLesson::SilenceDetectCue() const {
    if (UsesXiaozhiTeach()) {
        return "hãy khơi gợi";
    }
    if (IsTeaching()) {
        return "nhắc học viên nói";
    }
    return "khơi gợi lại";
}

const std::string& SpeakLesson::ComboPraise() const {
    return combo_praise_;
}

const std::string& SpeakLesson::ComboSpeak() const {
    return combo_script_.empty() ? combo_praise_ : combo_script_;
}

std::string SpeakLesson::CurrentTeachScript() {
    LoadNvs();
    const bool saved_brief = briefing_pending_;
    std::string card = CardJsonAt(index_, true);
    briefing_pending_ = saved_brief;
    cJSON* root = cJSON_Parse(card.c_str());
    if (!root) {
        return CurrentTeachEn();
    }
    std::string out;
    cJSON* speech = cJSON_GetObjectItem(root, "speech");
    if (cJSON_IsArray(speech)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, speech) {
            const char* text = JsonStr(it, "text");
            if (!text || !text[0]) {
                continue;
            }
            if (!out.empty()) {
                out += " ";
            }
            out += text;
        }
    }
    if (out.empty()) {
        cJSON* teacher = cJSON_GetObjectItem(root, "teacher");
        if (cJSON_IsString(teacher) && teacher->valuestring) {
            out = teacher->valuestring;
        }
    }
    cJSON_Delete(root);
    const std::string en = CurrentTeachEn();
    if (out.empty()) {
        out = en;
    }
    if (!en.empty() && !ContainsEnglish(out, en)) {
        if (!out.empty()) {
            out += " ";
        }
        out += en;
        out += ". ";
        out += en;
        out += ".";
    }
    if (briefing_pending_ && !briefing_.empty()) {
        out = briefing_ + " " + out;
        briefing_pending_ = false;
    }
    return out;
}

bool SpeakLesson::HoldMicForIntro() const {
    return on_ && !done_ask_ && just_auto_advanced_ && !card_intro_heard_;
}

std::string SpeakLesson::CurrentTeachEn() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (!self.ask_q_.empty()) {
        return self.ask_q_;
    }
    if (!self.answer_en_.empty()) {
        return self.answer_en_;
    }
    if (!self.playlist_.empty() && self.index_ >= 0 &&
        self.index_ < static_cast<int>(self.playlist_.size())) {
        return self.playlist_[self.index_];
    }
    return "";
}

std::string SpeakLesson::XiaozhiSkipToolJson() const {
    auto& self = *const_cast<SpeakLesson*>(this);
    self.LoadNvs();
    if (!self.on_ || self.done_ask_) {
        return "";
    }
    return "{\"on\":true,\"phase\":\"card\",\"skip\":true,\"rule\":\"Đang học. "
           "Firmware tự hiện ảnh và chấm STT. CẤM status/card/show/heard/next/start. "
           "Chỉ nói. Gọi tool lặp = lỗi 400.\"}";
}

std::string SpeakLesson::XiaozhiTeachStatus() {
    LoadNvs();
    const int64_t now = esp_timer_get_time();
    if (xz_status_hits_ > 0 && (now - last_xz_status_us_) < 2000000) {
        xz_status_hits_++;
        ESP_LOGI(TAG, "Xiaozhi status repeat #%u (skip dump)", static_cast<unsigned>(xz_status_hits_));
        return "{\"on\":true,\"repeat\":true,\"rule\":\"Đã gửi rồi. CẤM gọi status/card/show lại. "
               "Nói teacher hoặc feedback vừa nhận. Ảnh đã hiện.\"}";
    }
    xz_status_hits_++;
    last_xz_status_us_ = now;
    xz_got_card_ = true;
    if (done_ask_) {
        return DoneJson();
    }
    if (feedback_pending_ && !last_feedback_.empty() && !feedback_spoken_ && combo_praise_.empty()) {
        cJSON* out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "on", true);
        cJSON_AddStringToObject(out, "phase", "card");
        cJSON_AddStringToObject(out, "next_action", "speak_feedback");
        cJSON_AddStringToObject(out, "feedback", last_feedback_.c_str());
        cJSON_AddStringToObject(out, "rule",
                                "Nói HẾT feedback. CẤM status/card/next. CẤM dạy thẻ mới.");
        char* printed = cJSON_PrintUnformatted(out);
        std::string result = printed ? printed : "{}";
        cJSON_free(printed);
        cJSON_Delete(out);
        return result;
    }
    std::string card = CardJsonAt(index_, true);
    cJSON* root = cJSON_Parse(card.c_str());
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "on", true);
    }
    cJSON_DeleteItemFromObject(root, "rule");
    if (!combo_praise_.empty()) {
        cJSON_DeleteItemFromObject(root, "feedback");
        cJSON_AddStringToObject(root, "feedback", combo_praise_.c_str());
        cJSON_DeleteItemFromObject(root, "next_action");
        cJSON_AddStringToObject(root, "next_action", "speak_praise_then_card");
        cJSON_AddStringToObject(root, "rule",
                                "Nói HẾT feedback rồi LIỀN speech[]/teacher thẻ mới, MỘT HƠI, không dừng. "
                                "Ảnh đã hiện. CẤM status/card/show lại. CẤM next trước heard.");
    } else if (just_auto_advanced_) {
        cJSON_DeleteItemFromObject(root, "next_action");
        cJSON_AddStringToObject(root, "next_action", "speak_new_card");
        cJSON_AddStringToObject(root, "rule",
                                "Ảnh đã hiện. Nói speech[]/teacher thẻ mới. CẤM status/card/show lại. "
                                "CẤM khen thẻ cũ. CẤM next trước heard.");
    } else {
        cJSON_AddStringToObject(root, "rule",
                                "Ảnh đã hiện. Nói speech[]/teacher. CẤM gọi status/card/show lại. "
                                "Chờ heard. CẤM next.");
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(root);
    return result;
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
    const_cast<SpeakLesson*>(this)->LoadNvs();
    return ActiveBase();
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

std::string SpeakLesson::SetTeachStt(bool english_only, const std::string& codes) {
    LoadNvs();
    stt_english_ = english_only;
    if (!codes.empty() || english_only) {
        stt_codes_ = SanitizeTeachSttCodes(codes);
    }
    SaveNvs();
    ESP_LOGI(TAG, "Teach STT english_only=%d codes=%s", english_only ? 1 : 0, stt_codes_.c_str());
    return StatusJson();
}

bool SpeakLesson::SttEnglishOnly() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), stt_english_;
}

std::string SpeakLesson::SttCodes() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), stt_codes_;
}

std::vector<std::string> SpeakLesson::TeachSttLanguageCodes() const {
    auto& self = const_cast<SpeakLesson&>(*this);
    self.LoadNvs();
    if (!self.on_ || !self.stt_english_) {
        return {};
    }
    if (self.dai_ == "xiaozhi") {
        return {};
    }
    return SplitTeachSttCodes(self.stt_codes_);
}

std::vector<std::string> SpeakLesson::TeachSttVocabulary() const {
    auto& self = const_cast<SpeakLesson&>(*this);
    self.LoadNvs();
    std::vector<std::string> out;
    auto add = [&](const std::string& word) {
        if (word.size() < 2 || word.size() > 40) {
            return;
        }
        for (const auto& seen : out) {
            if (seen == word) {
                return;
            }
        }
        out.push_back(word);
    };
    if (!self.answer_en_.empty()) {
        add(self.answer_en_);
    }
    if (self.cached_json_.empty()) {
        return out;
    }
    cJSON* root = cJSON_Parse(self.cached_json_.c_str());
    cJSON* cards = root ? cJSON_GetObjectItem(root, "cards") : nullptr;
    if (cJSON_IsArray(cards)) {
        const int n = cJSON_GetArraySize(cards);
        for (int i = 0; i < n && static_cast<int>(out.size()) < 40; ++i) {
            add(JsonStr(cJSON_GetArrayItem(cards, i), "en"));
        }
    }
    cJSON_Delete(root);
    return out;
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
    std::string u = TrimSlash(url);
    if (u.empty()) {
        return;
    }
    // One link only: http:// stays HTTP, https:// stays HTTPS. No other-host fallback.
    base_ = u;
    cloud_ = u;
    src_ = LooksHttps(u) ? 2 : 1;
    live_.clear();
    cached_json_.clear();
    cached_key_.clear();
    cached_act_json_.clear();
    cached_act_key_.clear();
    SaveNvs();
    ESP_LOGI(TAG, "Course URL %s (%s)", u.c_str(), LooksHttps(u) ? "https" : "http");
}

void SpeakLesson::SetCourseSources(const std::string& lan, const std::string& cloud,
                                   const std::string& source) {
    LoadNvs();
    if (!lan.empty()) {
        base_ = TrimSlash(lan);
    }
    if (!cloud.empty()) {
        cloud_ = TrimSlash(cloud);
    }
    if (cloud_.empty()) {
        cloud_ = kDefaultCloud;
    }
    if (!source.empty()) {
        src_ = ParseCourseSource(source);
    }
    live_.clear();
    cached_json_.clear();
    cached_key_.clear();
    cached_act_json_.clear();
    cached_act_key_.clear();
    SaveNvs();
    ESP_LOGI(TAG, "Course sources lan=%s cloud=%s src=%s", base_.c_str(), cloud_.c_str(),
             CourseSourceName(src_));
}

std::string SpeakLesson::JoinOn(const std::string& host, const std::string& path) const {
    if (path.empty()) {
        return host;
    }
    if (path[0] == '/') {
        return host + path;
    }
    return host + "/" + path;
}

std::string SpeakLesson::ActiveBase() const {
    if (!live_.empty()) {
        return live_;
    }
    if (src_ == 2 && !cloud_.empty()) {
        return cloud_;
    }
    return base_.empty() ? kDefaultLan : base_;
}

void SpeakLesson::FillSourceOrder(std::string* first, std::string* second) const {
    const std::string lan = base_.empty() ? std::string(kDefaultLan) : base_;
    const std::string cld = cloud_.empty() ? std::string(kDefaultCloud) : cloud_;
    std::string pref = (src_ == 2) ? cld : lan;
    std::string other = (src_ == 2) ? lan : cld;
    if (other == pref) {
        other.clear();
    }
    if (!live_.empty()) {
        if (live_ == pref) {
            *first = live_;
            *second = other;
            return;
        }
        if (live_ == other) {
            *first = live_;
            *second = pref;
            return;
        }
    }
    *first = pref;
    *second = other;
}

std::string SpeakLesson::SwapCourseHost(const std::string& url) const {
    const std::string lan = base_.empty() ? std::string(kDefaultLan) : base_;
    const std::string cld = cloud_.empty() ? std::string(kDefaultCloud) : cloud_;
    if (!lan.empty() && url.compare(0, lan.size(), lan) == 0 && !cld.empty() && cld != lan) {
        return cld + url.substr(lan.size());
    }
    if (!cld.empty() && url.compare(0, cld.size(), cld) == 0 && !lan.empty() && lan != cld) {
        return lan + url.substr(cld.size());
    }
    return "";
}

void SpeakLesson::PinLiveFromUrl(const std::string& url) {
    const std::string lan = base_.empty() ? std::string(kDefaultLan) : base_;
    const std::string cld = cloud_.empty() ? std::string(kDefaultCloud) : cloud_;
    if (!lan.empty() && url.compare(0, lan.size(), lan) == 0) {
        live_ = lan;
    } else if (!cld.empty() && url.compare(0, cld.size(), cld) == 0) {
        live_ = cld;
    }
}

std::string SpeakLesson::JoinUrl(const std::string& path) const {
    return JoinOn(ActiveBase(), path);
}

std::string SpeakLesson::FetchText(const std::string& url) {
    PauseAfeForHttp pause_afe;
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
    // ReadAll() deadlocks in HttpClient when the body exceeds 8KB.
    // V5 speak JSON is 63–104KB; the old 64KB cap truncated u02+ and broke next-unit load.
    http->SetTimeout(LooksHttps(url) ? 20000 : 4000);
    http->SetKeepAlive(false);
    http->SetHeader("Connection", "close");
    http->SetHeader("Accept", "application/json,*/*");
    http->SetHeader("Accept-Encoding", "identity");
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
    const size_t announced = http->GetBodyLength();
    constexpr size_t kMaxJson = 192 * 1024;
    if (announced > kMaxJson) {
        ESP_LOGW(TAG, "GET too large %s %u", url.c_str(), (unsigned)announced);
        http->Close();
        return "";
    }
    const size_t want = (announced > 0) ? announced : kMaxJson;
    std::string body;
    if (announced > 0) {
        body.reserve(announced);
    }
    char buf[1024];
    while (body.size() < want) {
        int n = http->Read(buf, sizeof(buf));
        if (n < 0) {
            ESP_LOGW(TAG, "GET read timeout %s (%u/%u bytes)", url.c_str(),
                     (unsigned)body.size(), (unsigned)announced);
            break;
        }
        if (n == 0) {
            break;
        }
        body.append(buf, static_cast<size_t>(n));
    }
    http->Close();
    if (announced > 0 && body.size() < announced) {
        ESP_LOGW(TAG, "GET truncated %s got %u want %u", url.c_str(),
                 (unsigned)body.size(), (unsigned)announced);
        return "";
    }
    if (body.size() >= 1 && (body[0] == '<' || body.find("<!doctype") == 0 || body.find("<!DOCTYPE") == 0)) {
        ESP_LOGW(TAG, "GET got HTML not JSON %s (%u bytes)", url.c_str(), (unsigned)body.size());
        return "";
    }
    ESP_LOGI(TAG, "GET ok %s (%u bytes)", url.c_str(), (unsigned)body.size());
    return body;
}

std::string SpeakLesson::FetchFromSources(const std::string& path) {
    std::string first;
    std::string second;
    FillSourceOrder(&first, &second);
    const std::string* hosts[2] = {&first, &second};
    for (const std::string* host : hosts) {
        if (!host || host->empty()) {
            continue;
        }
        std::string body = FetchText(JoinOn(*host, path));
        if (!body.empty()) {
            live_ = *host;
            ESP_LOGI(TAG, "course live=%s path=%s", live_.c_str(), path.c_str());
            return body;
        }
    }
    return "";
}

bool SpeakLesson::EnsureUnitLoaded() {
    LoadNvs();
    std::string key = level_ + "/" + unit_;
    if (cached_key_ == key && !cached_json_.empty()) {
        return true;
    }
    const std::string path = "speak/" + level_ + "/" + unit_ + ".json";
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string body = FetchFromSources(path);
        if (body.empty()) {
            ESP_LOGW(TAG, "empty unit json %s try=%d", path.c_str(), attempt);
            continue;
        }
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            ESP_LOGW(TAG, "unit json parse fail %s bytes=%u try=%d", path.c_str(),
                     (unsigned)body.size(), attempt);
            continue;
        }
        cJSON_Delete(root);
        cached_json_ = std::move(body);
        cached_key_ = key;
        return true;
    }
    return false;
}

std::string SpeakLesson::CardJsonAt(int index, bool with_teacher) {
    if (!EnsureUnitLoaded()) {
        return "{\"error\":\"Khong tai duoc JSON. Bat may :8765 hoac dung https://emun.pages.dev.\"}";
    }
    cJSON* root = cJSON_Parse(cached_json_.c_str());
    if (!root) {
        return "{\"error\":\"JSON loi\"}";
    }
    if (!activity_id_.empty()) {
        EnsureActivitiesLoaded();
        if (playlist_.empty()) {
            BuildPlaylist();
        }
    }
    int total = LessonTotal();
    if (total <= 0) {
        total = CardCount(root);
    }
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
    cJSON* card = nullptr;
    std::string focus_id;
    if (IsFlip()) {
        focus_id = FlipFocusCardId();
        if (!focus_id.empty()) {
            card = FindCardById(root, focus_id.c_str());
        }
    } else if (!playlist_.empty() && index < static_cast<int>(playlist_.size())) {
        card = FindCardById(root, playlist_[index].c_str());
    }
    if (!card && !IsFlip()) {
        card = CardAt(root, index);
    }
    LoadSentenceItem(index);
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
        const bool want_game = image_field_ == "game_image";
        const char* game_url = str("game_image");
        const bool has_game = game_url && game_url[0];
        const bool graded_ok = !want_game || has_game;
        std::string image;
        if (want_game && has_game) {
            image = JoinUrl(GamePathFromUrl(game_url, level_, unit_, id));
        } else {
            image = JoinUrl(std::string("speak/") + level_ + "/" + unit_ + "/" + id + ".jpg");
        }
        const bool is_ask = type && strcmp(type, "ask") == 0;
        const bool hide_answer = !last_reveal_ &&
                                 (is_ask || activity_type_ == "complete_sentence" ||
                                  activity_type_ == "name_picture" || activity_type_ == "flip_guess" ||
                                  activity_type_ == "missing_picture" || activity_type_ == "unit_review");
        std::string teacher_out;
        AppendTeachSpeech(out, card, type, en, vi, teacher ? teacher : "", hide_answer, last_reveal_,
                          with_teacher, &teacher_out);
        if (is_ask && !ask_q_.empty()) {
            cJSON_AddStringToObject(out, "question", ask_q_.c_str());
            if (with_teacher && teacher_out.find(ask_q_) == std::string::npos) {
                if (!teacher_out.empty()) {
                    teacher_out += " ";
                }
                teacher_out += ask_q_;
                teacher_out += " ";
                teacher_out += ask_q_;
            }
        }
        if (with_teacher && briefing_pending_ && !briefing_.empty()) {
            teacher_out = briefing_ + " " + teacher_out;
            briefing_pending_ = false;
        }
        cJSON* explain_wrap = nullptr;
        if (explain_level_ > 0 && !is_ask) {
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
                        explain_wrap = cJSON_CreateObject();
                        if (cJSON_IsString(vi_e)) {
                            cJSON_AddStringToObject(explain_wrap, "vi", vi_e->valuestring);
                        }
                        if (cJSON_IsString(en_e)) {
                            cJSON_AddStringToObject(explain_wrap, "en", en_e->valuestring);
                        }
                    }
                }
            }
        }
        cJSON_AddStringToObject(out, "id", id);
        cJSON_AddStringToObject(out, "type", type);
        if (hide_answer) {
            cJSON_AddStringToObject(out, "en", pattern_en_.empty() ? "" : pattern_en_.c_str());
        } else {
            cJSON_AddStringToObject(out, "en", en);
        }
        cJSON_AddStringToObject(out, "vi", hide_answer ? "" : vi);
        if (with_teacher) {
            cJSON_AddStringToObject(out, "teacher", teacher_out.c_str());
        }
        cJSON_AddStringToObject(out, "image", image.c_str());
        if (has_game) {
            cJSON_AddStringToObject(out, "game_image", JoinUrl(GamePathFromUrl(game_url, level_, unit_, id)).c_str());
        }
        if (!pattern_en_.empty()) {
            cJSON_AddStringToObject(out, "pattern_en", pattern_en_.c_str());
        }
        if (last_reveal_ && !answer_en_.empty()) {
            cJSON_AddStringToObject(out, "answer_en", answer_en_.c_str());
        }
        cJSON_AddBoolToObject(out, "hide_answer", hide_answer);
        cJSON_AddBoolToObject(out, "graded", scoring_graded_ && graded_ok);
        if (explain_wrap) {
            cJSON_AddItemToObject(out, "explain", explain_wrap);
        }
        AppendActivityFields(out, last_reveal_);
        cJSON_AddBoolToObject(out, "done", false);
        cJSON_AddBoolToObject(out, "finished", index_ >= total);
    } else if (IsFlip()) {
        std::string teacher_out = TeacherForType("word", "", "", "", last_reveal_);
        if (with_teacher && briefing_pending_ && !briefing_.empty()) {
            teacher_out = briefing_ + " " + teacher_out;
            briefing_pending_ = false;
        }
        if (with_teacher) {
            cJSON_AddStringToObject(out, "teacher", teacher_out.c_str());
        }
        cJSON_AddBoolToObject(out, "hide_answer", !last_reveal_);
        cJSON_AddBoolToObject(out, "graded", scoring_graded_);
        AppendActivityFields(out, last_reveal_);
        cJSON_AddBoolToObject(out, "done", flip_phase_ >= 4);
        cJSON_AddBoolToObject(out, "finished", flip_phase_ >= 4);
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
                               const std::string& backend, bool force_reconnect,
                               const std::string& activity) {
    LoadNvs();
    if (!backend.empty()) {
        dai_ = NormalizeTeachBackend(backend);
    }
    InheritDailyAiIfNeeded();
    if (InLiveVoiceSession() && on_ && !done_ask_ && dai_ == "xiaozhi" && !force_reconnect) {
        ESP_LOGI(TAG, "Start ignored: already teaching Xiaozhi");
        return XiaozhiSkipToolJson();
    }
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
    if (!activity.empty()) {
        activity_id_ = activity;
    } else {
        activity_id_.clear();
        activity_type_.clear();
        image_field_.clear();
        playlist_.clear();
        cached_act_json_.clear();
        cached_act_key_.clear();
    }
    attempts_ = 0;
    item_phase_ = 2;
    last_reveal_ = false;
    last_correct_ = false;
    last_hint_.clear();
    last_verdict_.clear();
    last_feedback_.clear();
    last_heard_.clear();
    last_match_ = 0;
    wait_for_speech_ = false;
    feedback_pending_ = false;
    feedback_spoken_ = false;
    advance_after_feedback_ = false;
    just_auto_advanced_ = false;
    needs_review_act_ = false;
    items_correct_ = 0;
    items_wrong_ = 0;
    item_ok_ = false;
    hidden_id_.clear();
    group_ids_.clear();
    activity_locked_ = false;
    ResetFlipSession();
    const std::string want_key = level_ + "/" + unit_;
    if (cached_key_ != want_key) {
        cached_json_.clear();
        cached_key_.clear();
        last_shown_url_.clear();
    }
    const bool live = InLiveVoiceSession();
    const bool from_ui = force_reconnect || !live;
    if (from_ui && live) {
        ESP_LOGI(TAG, "Voice start: close chat, then load course like the UI button");
        Application::GetInstance().ParkVoiceForLessonStart();
    }
    if (from_ui) {
        CourseLoadGuard loading(true);
        if (!EnsureUnitLoaded()) {
            return "{\"success\":false,\"error\":\"Khong tai JSON. Thu may LAN :8765 hoac https://emun.pages.dev.\"}";
        }
        EnsureActivitiesLoaded();
        if (activity_id_.empty() && !cached_act_json_.empty()) {
            cJSON* pack = cJSON_Parse(cached_act_json_.c_str());
            activity_id_ = FirstActivityId(pack);
            cJSON_Delete(pack);
        }
        if (!activity_id_.empty()) {
            if (!EnsureActivitiesLoaded()) {
                return "{\"success\":false,\"error\":\"Khong tai activities.json. Thu LAN :8765 hoac Pages.\"}";
            }
            BuildPlaylist();
            if (activity_type_ == "unit_review" && !ReviewUnlocked()) {
                activity_locked_ = true;
                return "{\"success\":false,\"error\":\"Chua xong bai bat buoc de mo kiem tra cuoi.\",\"locked\":true}";
            }
            SpeakProgress::Get().MarkOpened(unit_, CurrentActIndex(), 0);
        }
        briefing_ = BuildBriefing();
        briefing_pending_ = !briefing_.empty();
        int total_pre = LessonTotal();
        if (index >= 0) {
            index_ = index;
        } else if (done_ask_ || (!on_ && total_pre > 0 && index_ >= total_pre)) {
            index_ = 0;
        }
        if (index_ < 0) {
            index_ = 0;
        }
        if (total_pre > 0 && index_ > total_pre) {
            index_ = total_pre;
        }
        // JSON only under the overlay. JPEG + reconnect together was watchdog-rebooting
        // after "Đang tải khóa học" finished (voice start and UI start).
        if (auto* otto = dynamic_cast<OttoEmojiDisplay*>(Board::GetInstance().GetDisplay())) {
            otto->PauseCourseLoadingTicks();
        }
        loading.KeepVisible();
    } else {
        ESP_LOGI(TAG, "Start during live voice: JSON on next card/show");
    }
    int total = LessonTotal();
    const bool was_on = on_;
    if (!from_ui) {
        if (index >= 0) {
            index_ = index;
        } else if (done_ask_ || (!was_on && total > 0 && index_ >= total)) {
            index_ = 0;
        }
        if (index_ < 0) {
            index_ = 0;
        }
        if (total > 0 && index_ > total) {
            index_ = total;
        }
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
        if (need_switch || want != have) {
            ESP_LOGI(TAG, "Start: switch to teaching identity and greet");
            SwitchIdentity();
        } else {
            ShowCurrent();
            HideCourseLoadingFace();
            if (force_reconnect || live) {
                ESP_LOGI(TAG, "Start: already teaching identity, greet without reconnect");
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().WakeWordInvoke("Hi,Jason");
                });
            }
        }
    } else if (need_switch || want != have) {
        identity_pending_ = true;
        ESP_LOGI(TAG, "Start during live voice: identity after this turn");
    }
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    if (from_ui) {
        just_auto_advanced_ = true;
        card_intro_heard_ = false;
        return CurrentCardJson();
    }
    if (dai_ == "xiaozhi") {
        xz_status_hits_ = 0;
        last_xz_status_us_ = 0;
        return XiaozhiTeachStatus();
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

std::string SpeakLesson::Next(bool force_skip, bool show_now) {
    LoadNvs();
    if (on_ && done_ask_) {
        return DoneJson();
    }
    if (!EnsureUnitLoaded()) {
        return "{\"error\":\"Khong tai JSON\"}";
    }
    if (!force_skip && just_auto_advanced_ && !item_ok_) {
        ESP_LOGI(TAG, "Next ignored: already auto-advanced, wait for heard");
        if (dai_ == "xiaozhi") {
            return "{\"ok\":true,\"skip\":true,\"rule\":\"Đã sang thẻ. CẤM next. Nói teacher.\"}";
        }
        ShowCurrent();
        return CurrentCardJson();
    }
    if (!force_skip && feedback_pending_ && !feedback_spoken_) {
        ESP_LOGI(TAG, "Next blocked: speak feedback first");
        if (dai_ == "xiaozhi") {
            return "{\"ok\":true,\"skip\":true,\"rule\":\"CẤM next. Nói feedback. Firmware tự sang thẻ.\"}";
        }
        ShowCurrent();
        return CurrentCardJson();
    }
    if (!force_skip && feedback_pending_ && feedback_spoken_ && !advance_after_feedback_) {
        if (item_ok_ || last_correct_ || last_verdict_ == "near" || last_verdict_ == "pass_on") {
            ESP_LOGI(TAG, "Next after pass: advance card");
            feedback_pending_ = false;
            feedback_spoken_ = false;
            advance_after_feedback_ = false;
        } else if (attempts_ >= 2 && !item_ok_) {
            ESP_LOGI(TAG, "Next after retries: praise and pass to next card");
            last_verdict_ = "pass_on";
            last_feedback_ = BuildFeedback("pass_on", last_heard_);
            last_hint_ = last_feedback_;
            last_reveal_ = true;
            needs_review_act_ = true;
            item_ok_ = true;
            wait_for_speech_ = false;
            item_phase_ = 5;
            feedback_pending_ = true;
            feedback_spoken_ = false;
            advance_after_feedback_ = true;
            ShowCurrent();
            return CurrentCardJson();
        } else {
            feedback_pending_ = false;
            feedback_spoken_ = false;
            wait_for_speech_ = true;
            item_phase_ = 3;
            ShowCurrent();
            return CurrentCardJson();
        }
    }
    if (feedback_pending_ && feedback_spoken_ && advance_after_feedback_) {
        feedback_pending_ = false;
        feedback_spoken_ = false;
        advance_after_feedback_ = false;
    }
    if (!force_skip && WaitsForSpeech() && !item_ok_ && !done_ask_) {
        wait_for_speech_ = true;
        item_phase_ = 3;
        last_verdict_ = last_verdict_.empty() ? "silence" : last_verdict_;
        last_feedback_ = BuildFeedback(last_verdict_ == "unclear" ? "uncertain" : "silence", "");
        last_hint_ = last_feedback_;
        feedback_pending_ = true;
        feedback_spoken_ = false;
        advance_after_feedback_ = false;
        ShowCurrent();
        return CurrentCardJson();
    }
    if (force_skip && WaitsForSpeech() && !item_ok_) {
        needs_review_act_ = true;
        feedback_pending_ = false;
        feedback_spoken_ = false;
        advance_after_feedback_ = false;
    }
    if (!activity_id_.empty()) {
        EnsureActivitiesLoaded();
        if (playlist_.empty()) {
            BuildPlaylist();
        }
        if (activity_type_ == "flip_guess" && flip_deck_.empty()) {
            FlipInitFromActivity();
        }
    }
    if (IsFlip()) {
        if (flip_phase_ == 1) {
            FlipAdvancePreview();
            ShowCurrent();
            return CurrentCardJson();
        }
        if (flip_phase_ == 3 && !active_card_id_.empty()) {
            FlipResolveCurrent(false, false, true);
            ShowCurrent();
            return CurrentCardJson();
        }
        ShowCurrent();
        return CurrentCardJson();
    }
    int total = LessonTotal();
    if (index_ < total) {
        index_++;
    }
    attempts_ = 0;
    last_reveal_ = false;
    last_correct_ = false;
    last_hint_.clear();
    last_verdict_.clear();
    last_feedback_.clear();
    last_heard_.clear();
    last_match_ = 0;
    wait_for_speech_ = false;
    feedback_pending_ = false;
    feedback_spoken_ = false;
    advance_after_feedback_ = false;
    if (activity_type_ == "missing_picture") {
        item_phase_ = 1;
    } else if (activity_type_ == "flip_guess") {
        item_phase_ = 2;
    } else {
        item_phase_ = 2;
    }
    item_ok_ = false;
    just_auto_advanced_ = true;
    card_intro_heard_ = false;
    intro_recue_ = 0;
    skip_turn_done_ = false;
    xz_status_hits_ = 0;
    last_xz_status_us_ = 0;
    on_ = true;
    if (index_ >= total) {
        done_ask_ = true;
        teach_armed_ = true;
        const bool mastered = !needs_review_act_ && items_wrong_ == 0;
        SpeakProgress::Get().MarkItem(unit_, CurrentActIndex(),
                                      static_cast<uint8_t>(index_ > 0 ? index_ - 1 : 0),
                                      true, true, needs_review_act_, mastered);
        SaveNvs();
        ESP_LOGI(TAG, "Activity/unit done: ask restart %s/%s %s", level_.c_str(), unit_.c_str(),
                 activity_id_.c_str());
        return DoneJson();
    }
    SpeakProgress::Get().MarkOpened(unit_, CurrentActIndex(), static_cast<uint8_t>(index_));
    SaveNvs();
    if (show_now) {
        ShowCurrent();
    }
    return CurrentCardJson();
}

std::string SpeakLesson::CurrentCardJson() {
    LoadNvs();
    xz_got_card_ = true;
    if (on_ && done_ask_) {
        return DoneJson();
    }
    intro_pending_ = false;
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
    if (!activity_id_.empty() && !cached_act_json_.empty() && playlist_.empty()) {
        BuildPlaylist();
    }
    if (!cached_json_.empty()) {
        cJSON* root = cJSON_Parse(cached_json_.c_str());
        if (root) {
            total = LessonTotal();
            if (total <= 0) {
                total = playlist_.empty() ? CardCount(root) : static_cast<int>(playlist_.size());
            }
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
    const bool need_intro = false;
    const bool done = on_ && done_ask_;
    cJSON_AddBoolToObject(out, "need_intro", need_intro);
    cJSON_AddBoolToObject(out, "finished", done || (total > 0 && index_ >= total));
    cJSON_AddStringToObject(out, "phase", done ? "done" : (need_intro ? "intro" : "card"));
    if (done) {
        std::string nxt_act = NextActivityId();
        std::string nxt = nxt_act.empty() ? NextUnitId(unit_) : unit_;
        cJSON_AddStringToObject(out, "say", DoneSay().c_str());
        if (!nxt_act.empty()) {
            cJSON_AddStringToObject(out, "next_activity", nxt_act.c_str());
        }
        if (!nxt.empty() && nxt_act.empty()) {
            cJSON_AddStringToObject(out, "next_unit", nxt.c_str());
        }
    } else if (briefing_pending_ && !briefing_.empty()) {
        cJSON_AddStringToObject(out, "say", briefing_.c_str());
    } else if (need_intro) {
        cJSON_AddStringToObject(out, "say", kIntroSay);
    }
    cJSON_AddStringToObject(out, "base", ActiveBase().c_str());
    cJSON_AddStringToObject(out, "lan", base_.c_str());
    cJSON_AddStringToObject(out, "cloud", cloud_.c_str());
    cJSON_AddStringToObject(out, "source", CourseSourceName(src_));
    cJSON_AddStringToObject(out, "live", live_.c_str());
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
    cJSON_AddBoolToObject(out, "stt_english", stt_english_);
    cJSON_AddStringToObject(out, "stt_codes", stt_codes_.c_str());
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
    std::string daily_prompt = XiaozhiDailyPrompt();
    cJSON_AddStringToObject(out, "xiaozhi_daily_prompt", daily_prompt.c_str());
    cJSON_AddBoolToObject(out, "sub_robot", sub_robot_);
    cJSON_AddBoolToObject(out, "sub_stt", sub_stt_);
    cJSON_AddStringToObject(out, "explain_level",
                            explain_level_ >= 2 ? "detailed" : (explain_level_ == 1 ? "basic" : "minimal"));
    AppendActivityFields(out, false);
    auto& prog = SpeakProgress::Get();
    cJSON_AddNumberToObject(out, "student_slot", prog.Slot());
    cJSON_AddStringToObject(out, "assessment", SpeakProgress::AssessName(prog.Assessment()));
    cJSON_AddStringToObject(out, "progress_hex", prog.HexBlob().c_str());
    cJSON_AddNumberToObject(out, "acts_completed", prog.CompletedCount());
    cJSON_AddNumberToObject(out, "acts_mastered", prog.MasteredCount());
    cJSON* stats = cJSON_AddObjectToObject(out, "stats");
    cJSON_AddNumberToObject(stats, "life_turns", prog.LifeTurns());
    cJSON_AddNumberToObject(stats, "life_words", prog.LifeWords());
    cJSON_AddNumberToObject(stats, "life_phrases", prog.LifePhrases());
    cJSON_AddNumberToObject(stats, "life_minutes", static_cast<int>(prog.LifeSeconds() / 60));
    cJSON_AddNumberToObject(stats, "today_turns", prog.TodayTurns());
    cJSON_AddNumberToObject(stats, "today_words", prog.TodayWords());
    cJSON_AddNumberToObject(stats, "today_phrases", prog.TodayPhrases());
    cJSON_AddNumberToObject(stats, "today_minutes", prog.TodaySeconds() / 60);
    int week_min = 0;
    cJSON* week = cJSON_AddArrayToObject(stats, "week_minutes");
    for (int d = 0; d < 7; ++d) {
        const int m = prog.WeekSeconds(d) / 60;
        week_min += m;
        cJSON_AddItemToArray(week, cJSON_CreateNumber(m));
    }
    cJSON_AddNumberToObject(stats, "week_minutes_total", week_min);
    cJSON* students = cJSON_AddArrayToObject(out, "students");
    cJSON* row = cJSON_CreateObject();
    cJSON_AddNumberToObject(row, "slot", 0);
    std::string nm = prog.StudentName(0);
    if (nm.empty()) {
        nm = WebSocketControlServer::GetStudentName();
    }
    cJSON_AddStringToObject(row, "name", nm.c_str());
    cJSON_AddItemToArray(students, row);
    char* printed = cJSON_PrintUnformatted(out);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(out);
    return result;
}

std::string SpeakLesson::UnitJson() {
    if (!EnsureUnitLoaded()) {
        return "{\"error\":\"Khong tai JSON. Thu LAN :8765 hoac https://emun.pages.dev.\"}";
    }
    return cached_json_;
}

bool SpeakLesson::ShowImage(const std::string& url) {
    if (url.empty()) {
        return false;
    }
    if (url == last_shown_url_) {
        ESP_LOGI(TAG, "ShowImage skip duplicate %s", url.c_str());
        return true;
    }
    if (ShowImageOnce(url)) {
        PinLiveFromUrl(url);
        return true;
    }
    const std::string alt = SwapCourseHost(url);
    if (!alt.empty() && ShowImageOnce(alt)) {
        PinLiveFromUrl(alt);
        return true;
    }
    return false;
}

bool SpeakLesson::ShowImageOnce(const std::string& url) {
    PauseAfeForHttp pause_afe;
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
    http->SetTimeout(LooksHttps(url) ? 20000 : 4000);
    http->SetKeepAlive(false);
    http->SetHeader("Connection", "close");
    http->SetHeader("Accept-Encoding", "identity");
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

    if (jpeg_len < 4 || static_cast<unsigned char>(data[0]) != 0xff ||
        static_cast<unsigned char>(data[1]) != 0xd8) {
        ESP_LOGW(TAG, "ShowImage not JPEG %s (%u bytes)", url.c_str(), (unsigned)jpeg_len);
        heap_caps_free(data);
        return false;
    }

    try {
        display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(data, jpeg_len));
        last_shown_url_ = url;
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
    std::string card = CardJsonAt(index_, false);
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
    std::string nxt_act = NextActivityId();
    std::string nxt = nxt_act.empty() ? NextUnitId(unit_) : "";
    const std::string say = DoneSay();
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "on", true);
    cJSON_AddBoolToObject(out, "finished", true);
    cJSON_AddStringToObject(out, "phase", "done");
    cJSON_AddBoolToObject(out, "need_intro", false);
    cJSON_AddBoolToObject(out, "skip_image", true);
    cJSON_AddStringToObject(out, "say", say.c_str());
    cJSON_AddStringToObject(out, "teacher", say.c_str());
    cJSON_AddStringToObject(out, "level", level_.c_str());
    cJSON_AddStringToObject(out, "unit", unit_.c_str());
    cJSON_AddNumberToObject(out, "index", index_);
    if (!nxt_act.empty()) {
        cJSON_AddStringToObject(out, "next_activity", nxt_act.c_str());
    }
    if (!nxt.empty()) {
        cJSON_AddStringToObject(out, "next_unit", nxt.c_str());
    }
    cJSON_AddStringToObject(out, "message",
                            nxt_act.empty() && nxt.empty()
                                ? "Het chu de cuoi. Noi field say. Hoc lai: start index=0. Nghi: self.speak.off."
                                : "Het bai. Noi field say. Hoc tiep: firmware tu tai bai Emun ke. CAM otto.next_unit. Nghi: self.speak.off.");
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
    intro_pending_ = false;
    intro_hold_ = false;
    intro_ready_ = false;
    skip_turn_done_ = false;
    skip_turn_done_us_ = 0;
    xz_status_hits_ = 0;
    last_xz_status_us_ = 0;
    combo_praise_.clear();
    combo_script_.clear();
    xz_got_card_ = false;
    card_intro_heard_ = false;
    intro_recue_ = 0;
    xz_kick_teacher_ = (dai_ == "xiaozhi");
    xz_hold_tools_ = false;
    just_auto_advanced_ = true;
    xz_wrong_word_ = false;
    speak_begin_us_ = 0;
    LoadSentenceItem(index_);
    ShowCurrent();
    HideCourseLoadingFace();
    const char* en = !playlist_.empty() && index_ >= 0 && index_ < static_cast<int>(playlist_.size())
                         ? playlist_[index_].c_str()
                         : (answer_en_.empty() ? "?" : answer_en_.c_str());
    ESP_LOGI(TAG, "New talk session: first card %s + image, no confirm", en);
}

void SpeakLesson::OnAssistantTurnDone() {
    if (skip_turn_done_) {
        skip_turn_done_ = false;
        const int64_t age = esp_timer_get_time() - skip_turn_done_us_;
        if (age >= 0 && age < 1500000) {
            ESP_LOGI(TAG, "Turn done ignored: abort blip (%d ms)", static_cast<int>(age / 1000));
            return;
        }
        ESP_LOGI(TAG, "Skip flag stale (%d ms), treat as real turn", static_cast<int>(age / 1000));
    }
    if (intro_hold_ && !intro_ready_) {
        intro_ready_ = true;
        ESP_LOGI(TAG, "Intro spoken, next card may start the unit");
    }
    bool auto_go = false;
    if (on_ && feedback_pending_ && !feedback_spoken_) {
        feedback_spoken_ = true;
        auto_go = advance_after_feedback_ && !IsFlip() && combo_praise_.empty();
        if (!auto_go) {
            xz_hold_tools_ = false;
        }
        ESP_LOGI(TAG, "Feedback spoken; %s", auto_go ? "auto-advance" : "stay / wait");
    }
    if (on_ && IsFlip() && flip_phase_ == 1 && !feedback_pending_) {
        FlipAdvancePreview();
        ShowCurrent();
    }
    if (UsesXiaozhiTeach()) {
        OnXiaozhiTurnDone(auto_go);
    } else {
        OnGeminiTurnDone(auto_go);
    }
}

void SpeakLesson::OnXiaozhiTurnDone(bool auto_go) {
    if (auto_go) {
        just_auto_advanced_ = true;
        card_intro_heard_ = false;
        ContinueAfterFeedback();
        if (IsTeaching()) {
            Application::GetInstance().SendDirectCue("thẻ mới");
        } else {
            Application::GetInstance().SendDirectCue("hết bài");
        }
        return;
    }
    if (xz_kick_teacher_ && !xz_got_card_ && !feedback_pending_ && !xz_wrong_word_) {
        const int64_t spoke_ms =
            speak_begin_us_ > 0 ? (esp_timer_get_time() - speak_begin_us_) / 1000 : 0;
        if (spoke_ms > 0 && spoke_ms < 2500) {
            xz_kick_teacher_ = false;
            ESP_LOGI(TAG, "Xiaozhi short greet (%d ms), cue học ngay again", static_cast<int>(spoke_ms));
            Application::GetInstance().SendDirectCue("học ngay");
            return;
        }
    }
    if (on_ && just_auto_advanced_ && !feedback_pending_) {
        if (!card_intro_heard_ && intro_recue_ < 2 && IsTeaching()) {
            intro_recue_++;
            skip_turn_done_ = true;
            skip_turn_done_us_ = esp_timer_get_time();
            ESP_LOGI(TAG, "Xiaozhi card not spoken yet, cue thẻ mới (%u) expect=%s",
                     static_cast<unsigned>(intro_recue_), CurrentTeachEn().c_str());
            Application::GetInstance().SendDirectCue("thẻ mới");
            return;
        }
        just_auto_advanced_ = false;
        card_intro_heard_ = true;
        xz_hold_tools_ = false;
        intro_recue_ = 0;
        ESP_LOGI(TAG, "Xiaozhi intro done, accept speech");
        if (pending_show_) {
            Application::GetInstance().Schedule([]() {
                SpeakLesson::Get().FlushPendingShow();
            });
        }
    }
}

void SpeakLesson::OnGeminiTurnDone(bool auto_go) {
    if (auto_go && combo_praise_.empty()) {
        just_auto_advanced_ = true;
        card_intro_heard_ = false;
        ContinueAfterFeedback();
        Application::GetInstance().SendDirectCue(IsTeaching() ? "sang thẻ tiếp" : "hết bài");
        return;
    }
    if (on_ && just_auto_advanced_ && !feedback_pending_) {
        if (!card_intro_heard_ && intro_recue_ < 2 && IsTeaching()) {
            intro_recue_++;
            skip_turn_done_ = true;
            skip_turn_done_us_ = esp_timer_get_time();
            ESP_LOGI(TAG, "Gemini card not spoken yet, cue lại (%u) expect=%s",
                     static_cast<unsigned>(intro_recue_), CurrentTeachEn().c_str());
            Application::GetInstance().SendDirectCue("sang thẻ tiếp");
            return;
        }
        just_auto_advanced_ = false;
        card_intro_heard_ = true;
        combo_praise_.clear();
        combo_script_.clear();
        intro_recue_ = 0;
        ESP_LOGI(TAG, "Gemini intro done, accept speech");
        if (pending_show_) {
            Application::GetInstance().Schedule([]() {
                SpeakLesson::Get().FlushPendingShow();
            });
        }
    }
}

void SpeakLesson::OnSpeakBegin() {
    speak_begin_us_ = esp_timer_get_time();
}

void SpeakLesson::OnTtsText(const std::string& text) {
    if (!on_ || text.empty() || done_ask_) {
        return;
    }
    const std::string expect = CurrentTeachEn();
    if (just_auto_advanced_ && !expect.empty() && ContainsEnglish(text, expect)) {
        const bool praise_has = !combo_praise_.empty() && ContainsEnglish(combo_praise_, expect);
        if (!(praise_has && text.size() <= combo_praise_.size() + 48)) {
            card_intro_heard_ = true;
            xz_got_card_ = true;
        }
    }
    if (!UsesXiaozhiTeach()) {
        return;
    }
    if (expect.empty() || expect.find(' ') != std::string::npos) {
        return;
    }
    if (ContainsWord(text, expect)) {
        xz_got_card_ = true;
        return;
    }
    std::vector<std::string> others = playlist_;
    if (others.empty() && !cached_json_.empty()) {
        cJSON* root = cJSON_Parse(cached_json_.c_str());
        if (root) {
            cJSON* cards = cJSON_GetObjectItem(root, "cards");
            if (cJSON_IsArray(cards)) {
                const int n = cJSON_GetArraySize(cards);
                for (int i = 0; i < n; ++i) {
                    cJSON* c = cJSON_GetArrayItem(cards, i);
                    cJSON* id = c ? cJSON_GetObjectItem(c, "id") : nullptr;
                    if (cJSON_IsString(id) && id->valuestring) {
                        others.emplace_back(id->valuestring);
                    }
                }
            }
            cJSON_Delete(root);
        }
    }
    for (const auto& id : others) {
        if (id == expect || id.size() < 3 || id.find(' ') != std::string::npos) {
            continue;
        }
        if (!ContainsWord(text, id)) {
            continue;
        }
        ESP_LOGW(TAG, "Xiaozhi said '%s' but card is '%s' — abort", id.c_str(), expect.c_str());
        xz_wrong_word_ = true;
        xz_kick_teacher_ = false;
        xz_got_card_ = false;
        skip_turn_done_ = true;
        skip_turn_done_us_ = esp_timer_get_time();
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        Application::GetInstance().SendDirectCue("học ngay");
        return;
    }
}

void SpeakLesson::ContinueAfterFeedback() {
    Next(true, true);
    pending_show_ = false;
    just_auto_advanced_ = true;
}

void SpeakLesson::HandleDoneChoice(const std::string& text) {
    if (IsFirmwareCue(text)) {
        return;
    }
    const bool stop = ContainsWord(text, "nghỉ") || ContainsWord(text, "dừng") ||
                      ContainsWord(text, "không học") || ContainsWord(text, "tạm biệt") ||
                      ContainsWord(text, "giao tiếp") || ContainsWord(text, "stop") ||
                      ContainsWord(text, "bye");
    const bool restart = ContainsWord(text, "học lại") || ContainsWord(text, "từ đầu") ||
                         ContainsWord(text, "làm lại");
    const bool cont = ContainsWord(text, "học tiếp") || ContainsWord(text, "tiếp theo") ||
                      ContainsWord(text, "tiếp tục") || ContainsWord(text, "bài tiếp") ||
                      ContainsWord(text, "chủ đề tiếp") || ContainsWord(text, "qua bài") ||
                      ContainsWord(text, "sang bài") || ContainsWord(text, "next") ||
                      ContainsWord(text, "sẵn sàng") || ContainsWord(text, "unit tiếp") ||
                      ContainsWord(text, "tiếp đi") || ContainsWord(text, "bài mới") ||
                      ContainsWord(text, "học mới") || ContainsWord(text, "ok") ||
                      ContainsWord(text, "okay") || ContainsWord(text, "oke") ||
                      ContainsWord(text, "được") || ContainsWord(text, "yes") ||
                      ContainsWord(text, "ừ") || ContainsWord(text, "tiếp");
    if (stop && !restart) {
        ESP_LOGI(TAG, "Done choice: stop / daily");
        Stop();
        return;
    }
    if (restart && !cont) {
        ESP_LOGI(TAG, "Done choice: restart %s/%s %s", level_.c_str(), unit_.c_str(),
                 activity_id_.c_str());
        Start(level_, unit_, 0, "", true, activity_id_);
        return;
    }
    if (cont) {
        ESP_LOGI(TAG, "Done choice: next Emun lesson");
        ContinueNextLesson();
        return;
    }
    if (ContainsWord(text, "làm gì") || ContainsWord(text, "rồi sao")) {
        Application::GetInstance().SendDirectCue("hết bài");
    }
}

void SpeakLesson::OnSttText(const std::string& text) {
    if (!on_ || text.empty()) {
        return;
    }
    if (done_ask_) {
        HandleDoneChoice(text);
        return;
    }
    if (IsLessonFiller(text) || IsSttYoutubeGhost(text)) {
        const std::string en = CurrentTeachEn();
        const std::string fen = FoldEnglish(en);
        const bool keep_ok = ContainsEnglish(text, en) || fen == "ok" || fen == "okay" ||
                             fen == "yes";
        const bool keep_bye = (fen == "bye" || fen == "goodbye") && IsShortBye(FoldLoose(text));
        if (!keep_ok && !keep_bye) {
            ESP_LOGI(TAG, "STT ignored: filler/ghost (%s)", text.c_str());
            return;
        }
    }
    if (IsFirmwareCue(text)) {
        ESP_LOGI(TAG, "STT ignored: firmware cue (%s)", text.c_str());
        return;
    }
    if (IsFlip() && flip_phase_ != 3) {
        return;
    }
    if (!WaitsForSpeech() && activity_type_ != "name_picture" &&
        activity_type_ != "ask_answer" && activity_type_ != "complete_sentence") {
        return;
    }
    if (just_auto_advanced_) {
        ESP_LOGI(TAG, "STT ignored: waiting for new card intro (%s)", text.c_str());
        return;
    }
    if (item_ok_ || last_correct_ || advance_after_feedback_) {
        ESP_LOGI(TAG, "STT ignored: already passed (%s)", text.c_str());
        KickAfterGrade();
        return;
    }
    if (text == last_heard_ && feedback_pending_ && !feedback_spoken_) {
        return;
    }
    ESP_LOGI(TAG, "STT grade: %s", text.c_str());
    Evaluate(text, "", "voice");
    KickAfterGrade();
}

void SpeakLesson::KickAfterGrade() {
    if (!on_ || done_ask_) {
        return;
    }
    const bool go_next = advance_after_feedback_ && !IsFlip();
    if (go_next && just_auto_advanced_ && !combo_praise_.empty()) {
        return;
    }
    if (!go_next && (!feedback_pending_ || last_feedback_.empty() || feedback_spoken_)) {
        return;
    }
    if (UsesXiaozhiTeach()) {
        KickXiaozhiAfterGrade(go_next);
        return;
    }
    if (IsTeaching()) {
        KickGeminiAfterGrade(go_next);
        return;
    }
    skip_turn_done_ = true;
    skip_turn_done_us_ = esp_timer_get_time();
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
    Application::GetInstance().SendDirectCue("hết bài");
}

void SpeakLesson::KickXiaozhiAfterGrade(bool go_next) {
    xz_status_hits_ = 0;
    last_xz_status_us_ = 0;
    xz_hold_tools_ = true;
    if (go_next && !just_auto_advanced_) {
        ESP_LOGI(TAG, "Xiaozhi kick: praise from hearing, next card after that turn");
        just_auto_advanced_ = true;
        card_intro_heard_ = false;
        pending_show_ = true;
        return;
    }
    if (!go_next) {
        ESP_LOGI(TAG, "Xiaozhi kick: stay, no abort, model nudges from hearing");
    }
}

void SpeakLesson::KickGeminiAfterGrade(bool go_next) {
    if (go_next && !just_auto_advanced_) {
        ESP_LOGI(TAG, "Gemini kick: praise from hearing, next card after that turn");
        just_auto_advanced_ = true;
        card_intro_heard_ = false;
        pending_show_ = true;
        return;
    }
    if (!go_next) {
        ESP_LOGI(TAG, "Gemini kick: stay, cue chưa khớp");
        skip_turn_done_ = true;
        skip_turn_done_us_ = esp_timer_get_time();
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        Application::GetInstance().SendDirectCue("chưa khớp");
    }
}

void SpeakLesson::OnTalkIdle() {
    LoadNvs();
    if (identity_pending_ && on_) {
        identity_pending_ = false;
        teach_armed_ = true;
        ESP_LOGI(TAG, "Talk idle: apply teaching identity and greet");
        SwitchIdentity();
        return;
    }
    if (on_ && !done_ask_) {
        ESP_LOGI(TAG, "Talk idle: keep lesson image, do not restore eyes");
        return;
    }
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetPreviewImage(nullptr);
    }
    ESP_LOGI(TAG, "Talk idle: restored default eyes");
    if (on_) {
        if (!done_ask_) {
            teach_armed_ = false;
        }
        return;
    }
    if (style_new_ && (new_slot_ == 0 || teach_armed_)) {
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
    return std::string(kXiaozhiPrompt) + ProfileLine();
}

std::string SpeakLesson::XiaozhiPromptEn() const {
    return std::string(kXiaozhiPromptEn) + ProfileLineEn();
}

std::string SpeakLesson::XiaozhiDailyPrompt() const {
    return std::string(kXiaozhiDailyPrompt) + ProfileLine();
}

std::string SpeakLesson::DirectAiExtraPrompt() const {
    if (!IsOn()) {
        return "";
    }
    std::string extra = kDirectPrompt;
    extra += "Kịch bản nằm trong cue. Đọc một hơi, đừng tách briefing / thẻ thành hai lượt.\n";
    if (stt_english_) {
        extra += "STT đang ưu tiên tiếng Anh. Transcript heard phải viết English (father/uncle), "
                 "không viết tiếng Việt. Bé nói lệch giọng vẫn ghi từ tiếng Anh của thẻ.\n";
    }
    extra += ProfileLine();
    return extra;
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

std::string SpeakLesson::ActivityId() const {
    return const_cast<SpeakLesson*>(this)->LoadNvs(), activity_id_;
}

int SpeakLesson::LessonTotal() {
    if (IsFlip() && !flip_deck_.empty()) {
        return static_cast<int>(flip_deck_.size());
    }
    if (!playlist_.empty()) {
        return static_cast<int>(playlist_.size());
    }
    if (cached_json_.empty()) {
        return 0;
    }
    cJSON* root = cJSON_Parse(cached_json_.c_str());
    int n = CardCount(root);
    cJSON_Delete(root);
    return n;
}

int SpeakLesson::CurrentActIndex() const {
    int known = SpeakProgress::KnownActIndex(activity_id_);
    if (known >= 0) {
        return known;
    }
    if (cached_act_json_.empty()) {
        return 0;
    }
    cJSON* root = cJSON_Parse(cached_act_json_.c_str());
    if (!root) {
        return 0;
    }
    cJSON* acts = cJSON_GetObjectItem(root, "activities");
    int found = 0;
    if (cJSON_IsArray(acts)) {
        const int n = cJSON_GetArraySize(acts);
        for (int i = 0; i < n; ++i) {
            cJSON* act = cJSON_GetArrayItem(acts, i);
            if (activity_id_ == JsonStr(act, "id")) {
                found = i;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return found;
}

bool SpeakLesson::EnsureActivitiesLoaded() {
    LoadNvs();
    const std::string key = level_ + "/" + unit_;
    if (cached_act_key_ == key && !cached_act_json_.empty()) {
        return true;
    }
    const std::string path = "course/" + level_ + "/" + unit_ + ".activities.json";
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string body = FetchFromSources(path);
        if (body.empty()) {
            ESP_LOGW(TAG, "empty activities %s try=%d", path.c_str(), attempt);
            continue;
        }
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            ESP_LOGW(TAG, "activities parse fail %s bytes=%u try=%d", path.c_str(),
                     (unsigned)body.size(), attempt);
            continue;
        }
        cJSON_Delete(root);
        cached_act_json_ = std::move(body);
        cached_act_key_ = key;
        return true;
    }
    return false;
}

std::string SpeakLesson::BuildBriefing() const {
    std::string title_en;
    std::string title_vi;
    std::string goal;
    std::string act_title;
    std::string act_desc;
    if (!cached_json_.empty()) {
        cJSON* unit = cJSON_Parse(cached_json_.c_str());
        if (unit) {
            title_en = JsonStr(unit, "title_en");
            title_vi = JsonStr(unit, "title_vi");
            goal = JsonStr(unit, "goal_vi");
            cJSON_Delete(unit);
        }
    }
    if (!cached_act_json_.empty() && !activity_id_.empty()) {
        cJSON* pack = cJSON_Parse(cached_act_json_.c_str());
        cJSON* act = FindActivity(pack, activity_id_);
        if (act) {
            act_title = JsonStr(act, "title_vi");
            act_desc = JsonStr(act, "description_vi");
        }
        cJSON_Delete(pack);
    }
    std::string s = "Bạn có nhiệm vụ mới. ";
    if (!title_en.empty()) {
        s += "Chủ đề ";
        s += title_en;
        if (!title_vi.empty()) {
            s += " – ";
            s += title_vi;
        }
        s += ". ";
    } else if (!title_vi.empty()) {
        s += "Chủ đề ";
        s += title_vi;
        s += ". ";
    }
    if (!goal.empty()) {
        s += goal;
        if (s.back() != '.' && s.back() != '!' && s.back() != '?') {
            s += '.';
        }
        s += ' ';
    }
    if (!act_title.empty()) {
        s += "Bài này: ";
        s += act_title;
        s += ". ";
        if (!act_desc.empty()) {
            s += act_desc;
            if (s.back() != '.' && s.back() != '!' && s.back() != '?') {
                s += '.';
            }
            s += ' ';
        }
    }
    s += "Mình bắt đầu học luôn.";
    return s;
}

void SpeakLesson::BuildPlaylist() {
    playlist_.clear();
    activity_type_.clear();
    image_field_ = "image";
    scoring_graded_ = false;
    allow_personal_ = false;
    pattern_en_.clear();
    answer_en_.clear();
    if (activity_id_.empty() || cached_act_json_.empty()) {
        return;
    }
    cJSON* root = cJSON_Parse(cached_act_json_.c_str());
    if (!root) {
        return;
    }
    cJSON* act = FindActivity(root, activity_id_);
    if (!act) {
        cJSON_Delete(root);
        return;
    }
    activity_type_ = JsonStr(act, "type");
    image_field_ = JsonStr(act, "image_field");
    if (image_field_.empty()) {
        image_field_ = "image";
    }
    scoring_graded_ = (strcmp(JsonStr(act, "scoring"), "graded") == 0);
    allow_personal_ = cJSON_IsTrue(cJSON_GetObjectItem(act, "allow_personal_answers"));
    cJSON* ids = cJSON_GetObjectItem(act, "target_ids");
    if (cJSON_IsArray(ids)) {
        const int n = cJSON_GetArraySize(ids);
        for (int i = 0; i < n; ++i) {
            cJSON* it = cJSON_GetArrayItem(ids, i);
            if (cJSON_IsString(it) && it->valuestring) {
                playlist_.emplace_back(it->valuestring);
            }
        }
    }
    int group_size = 4;
    cJSON* gs = cJSON_GetObjectItem(act, "group_size");
    if (cJSON_IsNumber(gs) && gs->valueint >= 2) {
        group_size = gs->valueint;
    }
    const bool shuffle = cJSON_IsTrue(cJSON_GetObjectItem(act, "shuffle_each_round"));
    const bool flip = (activity_type_ == "flip_guess");
    if (!flip && shuffle && playlist_.size() > 1) {
        for (size_t i = playlist_.size() - 1; i > 0; --i) {
            const size_t j = static_cast<size_t>(esp_random() % (i + 1));
            std::swap(playlist_[i], playlist_[j]);
        }
    }
    const char* canonical = JsonStr(act, "id");
    if (canonical && canonical[0]) {
        activity_id_ = canonical;
    }
    if (activity_type_ == "missing_picture" && playlist_.size() >= static_cast<size_t>(group_size)) {
        std::vector<std::string> groups;
        for (size_t i = 0; i + static_cast<size_t>(group_size) - 1 < playlist_.size();
             i += static_cast<size_t>(group_size)) {
            groups.push_back(playlist_[i]);
        }
        if (!groups.empty()) {
            playlist_.swap(groups);
        }
    }
    cJSON_Delete(root);
    if (activity_type_ == "flip_guess") {
        item_phase_ = 1;
        FlipInitFromActivity();
    } else if (activity_type_ == "missing_picture") {
        item_phase_ = 1;
    } else {
        item_phase_ = 2;
    }
}

void SpeakLesson::LoadSentenceItem(int index) {
    pattern_en_.clear();
    answer_en_.clear();
    ask_q_.clear();
    group_ids_.clear();
    hidden_id_.clear();
    if (cached_json_.empty()) {
        return;
    }
    cJSON* cards_root = cJSON_Parse(cached_json_.c_str());
    std::string card_id;
    if (IsFlip()) {
        card_id = FlipFocusCardId();
    } else if (!playlist_.empty() && index >= 0 && index < static_cast<int>(playlist_.size())) {
        card_id = playlist_[index];
    }
    cJSON* card = card_id.empty() ? (IsFlip() ? nullptr : CardAt(cards_root, index))
                                  : FindCardById(cards_root, card_id.c_str());
    const char* en = JsonStr(card, "en");
    if (en && en[0]) {
        answer_en_ = en;
    }
    if (strcmp(JsonStr(card, "type"), "ask") == 0) {
        ask_q_ = ExtractAskQuestion(card);
    }
    if (!cached_act_json_.empty()) {
        cJSON* acts = cJSON_Parse(cached_act_json_.c_str());
        cJSON* act = FindActivity(acts, activity_id_);
        cJSON* items = act ? cJSON_GetObjectItem(act, "items") : nullptr;
        if (cJSON_IsArray(items)) {
            const int n = cJSON_GetArraySize(items);
            for (int i = 0; i < n; ++i) {
                cJSON* row = cJSON_GetArrayItem(items, i);
                if (card_id == JsonStr(row, "card_id")) {
                    pattern_en_ = JsonStr(row, "pattern_en");
                    const char* ans = JsonStr(row, "answer_en");
                    if (ans && ans[0]) {
                        answer_en_ = ans;
                    }
                    break;
                }
            }
        }
        if (activity_type_ == "missing_picture" && acts) {
            cJSON* ids = act ? cJSON_GetObjectItem(act, "target_ids") : nullptr;
            int gsz = 4;
            cJSON* gs = act ? cJSON_GetObjectItem(act, "group_size") : nullptr;
            if (cJSON_IsNumber(gs) && gs->valueint >= 2) {
                gsz = gs->valueint;
            }
            if (cJSON_IsArray(ids)) {
                const int n = cJSON_GetArraySize(ids);
                const int base = index * gsz;
                for (int i = 0; i < gsz && base + i < n; ++i) {
                    cJSON* it = cJSON_GetArrayItem(ids, base + i);
                    if (cJSON_IsString(it)) {
                        group_ids_.emplace_back(it->valuestring);
                    }
                }
                if (!group_ids_.empty()) {
                    const int pick = (SpeakProgress::UnitIndex(unit_) + index + SpeakProgress::Get().Slot()) %
                                     static_cast<int>(group_ids_.size());
                    hidden_id_ = group_ids_[pick];
                    if (!hidden_id_.empty()) {
                        cJSON* hid = FindCardById(cards_root, hidden_id_.c_str());
                        const char* hen = JsonStr(hid, "en");
                        if (hen && hen[0]) {
                            answer_en_ = hen;
                        }
                    }
                }
            }
        }
        cJSON_Delete(acts);
    }
    cJSON_Delete(cards_root);
}

bool SpeakLesson::ReviewUnlocked() {
    if (cached_act_json_.empty() && !EnsureActivitiesLoaded()) {
        return false;
    }
    cJSON* root = cJSON_Parse(cached_act_json_.c_str());
    if (!root) {
        return false;
    }
    std::vector<int> req;
    cJSON* arr = cJSON_GetObjectItem(root, "review_unlock_requires");
    if (cJSON_IsArray(arr)) {
        const int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; ++i) {
            cJSON* it = cJSON_GetArrayItem(arr, i);
            if (cJSON_IsString(it)) {
                int idx = SpeakProgress::KnownActIndex(it->valuestring);
                if (idx >= 0) {
                    req.push_back(idx);
                }
            }
        }
    }
    cJSON_Delete(root);
    return SpeakProgress::Get().AllCompleted(unit_, req);
}

std::string SpeakLesson::TeacherForType(const char* type, const char* en, const char* vi,
                                        const std::string& base_teacher, bool reveal) {
    const std::string word = en ? en : "";
    const std::string mean = vi ? vi : "";
    if (activity_type_ == "learn_vocab") {
        if (!base_teacher.empty()) {
            return SanitizePersona(base_teacher);
        }
        std::string t = "Nhìn hình. ";
        if (!mean.empty() && explain_level_ > 0) {
            t += mean + ". ";
        }
        t += "Mình nói 3 lần: " + word + ". " + word + ". " + word + ". Bạn nhắc lại nhé.";
        return t;
    }
    if (activity_type_ == "repeat_phrase") {
        if (!base_teacher.empty()) {
            return SanitizePersona(base_teacher);
        }
        return "Câu mẫu: " + word + (mean.empty() ? "" : (". Nghĩa: " + mean)) + ". Bạn nói lại cả câu.";
    }
    if (activity_type_ == "name_picture") {
        return reveal ? ("Đáp án: " + word + ". Bạn nói theo nhé.")
                      : "Bạn nhìn hình và gọi tên nhé.";
    }
    if (activity_type_ == "complete_sentence") {
        if (reveal) {
            return "Đáp án: " + (answer_en_.empty() ? word : answer_en_) + ".";
        }
        return "Bạn hoàn thành câu. Mẫu: " + (pattern_en_.empty() ? "..." : pattern_en_) +
               ". Đừng nghe đáp án trước.";
    }
    if (activity_type_ == "flip_guess") {
        if (flip_phase_ <= 1 || item_phase_ <= 1) {
            if (!word.empty()) {
                return "Đây là " + word + ".";
            }
            return "Trước tiên, mình cùng xem và nghe tên từng hình để nhớ nhé.";
        }
        if (reveal) {
            return word.empty() ? "Mình công bố đáp án rồi. Bạn chọn thẻ tiếp theo nhé."
                                : ("Đây là " + word + ".");
        }
        if (flip_phase_ == 3 && !active_card_id_.empty()) {
            return "Bạn nhìn hình này và nói tên bằng tiếng Anh nhé.";
        }
        return "Bây giờ các thẻ sẽ úp xuống. Bạn chọn một thẻ bất kỳ để mở nhé.";
    }
    if (activity_type_ == "missing_picture") {
        if (item_phase_ <= 1) {
            return "Nhìn lần lượt 4 hình, nhớ tên. Sau đó một hình sẽ biến mất.";
        }
        return reveal ? ("Hình biến mất là " + (answer_en_.empty() ? word : answer_en_) + ".")
                      : "Hình nào biến mất? Bạn gọi tên nhé.";
    }
    if (activity_type_ == "ask_answer") {
        if (reveal) {
            return base_teacher.empty() ? ("Ví dụ trả lời: " + word) : base_teacher;
        }
        if (!base_teacher.empty()) {
            return base_teacher + " Bạn trả lời theo bản thân, không cần nhắc lại câu hỏi.";
        }
        return "Mình hỏi. Bạn trả lời theo bản thân, không cần nhắc lại câu hỏi.";
    }
    if (activity_type_ == "unit_review") {
        if (type && strcmp(type, "ask") == 0) {
            if (!base_teacher.empty()) {
                return base_teacher;
            }
            return "Mình hỏi. Bạn trả lời theo bản thân.";
        }
        if (!pattern_en_.empty()) {
            return "Hoàn thành câu: " + pattern_en_;
        }
        return "Nhìn hình và nói. Không gợi đáp án.";
    }
    return SanitizePersona(base_teacher);
}

bool SpeakLesson::WaitsForSpeech() const {
    if (IsFlip() || done_ask_) {
        return false;
    }
    return activity_type_.empty() || activity_type_ == "learn_vocab" ||
           activity_type_ == "repeat_phrase";
}

std::string SpeakLesson::SanitizePersona(std::string text) const {
    auto replace_all = [](std::string* s, const char* from, const char* to) {
        if (!s || !from || !from[0]) {
            return;
        }
        const size_t n = strlen(from);
        const size_t m = strlen(to);
        size_t pos = 0;
        while ((pos = s->find(from, pos)) != std::string::npos) {
            s->replace(pos, n, to);
            pos += m;
        }
    };
    replace_all(&text, "chú robot", "mình");
    replace_all(&text, "Chú robot", "Mình");
    replace_all(&text, "con robot", "mình");
    replace_all(&text, "Con robot", "Mình");
    replace_all(&text, " robot ", " mình ");
    replace_all(&text, "Robot ", "Mình ");
    replace_all(&text, "Cảm ơn con", "Cảm ơn bạn");
    replace_all(&text, "cảm ơn con", "cảm ơn bạn");
    replace_all(&text, " con ơi", " bạn");
    replace_all(&text, "Con ơi", "Bạn");
    replace_all(&text, " con.", " bạn.");
    replace_all(&text, " con,", " bạn,");
    replace_all(&text, " con!", " bạn!");
    replace_all(&text, " con?", " bạn?");
    replace_all(&text, " Con ", " Bạn ");
    replace_all(&text, " của con", " của bạn");
    if (text.rfind("Con ", 0) == 0) {
        text.replace(0, 4, "Bạn ");
    }
    return text;
}

std::string SpeakLesson::BuildFeedback(const char* kind, const std::string& heard) const {
    const std::string en = answer_en_;
    const std::string said = !heard.empty() ? heard : en;
    const std::string label = said.empty() ? (en.empty() ? "tiếng Anh" : en) : said;
    const char* k = kind ? kind : "";
    const int pick = static_cast<int>((praise_i_ + index_) % 4);
    char buf[260];
    if (strcmp(k, "correct") == 0) {
        const char* a[] = {
            "Bạn làm tốt lắm! Mình nhận ra bạn nói %s.",
            "Giỏi quá! Bạn vừa nói %s.",
            "Hay lắm! Mình nghe %s rồi.",
            "Bạn làm tốt lắm! Mình nhận đúng %s.",
        };
        snprintf(buf, sizeof(buf), a[pick], label.c_str());
        return SanitizePersona(buf);
    }
    if (strcmp(k, "retry_success") == 0) {
        const char* a[] = {
            "Bạn nói lại được rồi! Giỏi quá! Mình nhận ra %s.",
            "Hay lắm! Lần này mình nhận đúng %s.",
            "Bạn làm tốt lắm! Mình nghe %s rõ hơn rồi.",
            "Giỏi quá! Bạn thử lại và mình nhận ra %s.",
        };
        snprintf(buf, sizeof(buf), a[pick], label.c_str());
        return SanitizePersona(buf);
    }
    if (strcmp(k, "near") == 0) {
        const char* a[] = {
            "Bạn nói gần đúng rồi! Giỏi quá! Từ tiếng Anh là %s. Mình sang câu sau nhé.",
            "Hay lắm! Mình nhận ra ý của bạn. Từ này là %s.",
            "Bạn làm tốt lắm! Gần đúng rồi. Mình đọc một lần: %s. Mình luyện tiếp nhé.",
            "Giỏi quá! Mình hiểu bạn. Tiếng Anh là %s.",
        };
        snprintf(buf, sizeof(buf), a[pick], en.empty() ? label.c_str() : en.c_str());
        return SanitizePersona(buf);
    }
    if (strcmp(k, "pass_on") == 0) {
        return "Bạn đã cố gắng lắm! Mình khen bạn rồi. Mình sang thẻ mới luôn nhé.";
    }
    if (strcmp(k, "incorrect") == 0) {
        const char* a[] = {
            "Bạn đã mạnh dạn nói rồi. Chưa phải từ mình cần. Mình đọc: %s.",
            "Bạn đã cố gắng. Mình chưa nhận ra đúng từ. Nghe mẫu: %s.",
            "Bạn đã lên tiếng rồi! Chưa khớp. Mình đọc chậm: %s.",
            "Bạn làm tốt khi đã nói. Điểm cần sửa: nói %s.",
        };
        snprintf(buf, sizeof(buf), a[pick], en.empty() ? "lại theo mình" : en.c_str());
        return SanitizePersona(buf);
    }
    if (strcmp(k, "uncertain") == 0) {
        return "Mình chưa nghe rõ. Bạn nói lại chậm một chút nhé. Lượt này không tính sai.";
    }
    if (strcmp(k, "silence") == 0) {
        const char* a[] = {
            "Mình vẫn chờ bạn. Hãy nói to một chút nhé.",
            "Bạn thử nói theo mình đi, không sao nếu chưa khớp.",
            "Mình nghe bạn nói rồi mới sang câu sau nhé.",
            "Bạn mở lời đi, mình đang lắng nghe.",
        };
        return a[pick];
    }
    if (strcmp(k, "heard") == 0) {
        return "Mình nghe bạn lên tiếng rồi. Nói rõ từ tiếng Anh giúp mình nhận ra nhé.";
    }
    return "";
}

std::string SpeakLesson::BuildComboScript() {
    std::string script = combo_praise_;
    const std::string en = CurrentTeachEn();
    if (!en.empty()) {
        if (!script.empty()) {
            script += " ";
        }
        script += en;
        script += ". ";
        script += en;
        script += ".";
    }
    script += " Bạn nói theo nhé.";
    return script;
}

void SpeakLesson::FlushPendingShow() {
    if (!pending_show_ || !on_ || done_ask_) {
        return;
    }
    pending_show_ = false;
    ShowCurrent();
}

void SpeakLesson::AppendTeachSpeech(cJSON* out, cJSON* card, const char* type, const char* en,
                                    const char* vi, const char* teacher, bool hide_answer,
                                    bool reveal, bool with_teacher, std::string* teacher_out) {
    std::string built;
    if (feedback_pending_ && !last_feedback_.empty()) {
        cJSON* speech_out = out ? cJSON_AddArrayToObject(out, "speech") : nullptr;
        auto add = [&](const char* lang, const char* text, const char* role) {
            if (!text || !text[0] || !speech_out) {
                return;
            }
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "lang", lang);
            cJSON_AddStringToObject(item, "text", text);
            cJSON_AddStringToObject(item, "role", role);
            cJSON_AddItemToArray(speech_out, item);
        };
        add("vi-VN", last_feedback_.c_str(), "praise");
        if (last_match_ != SpeakProgress::kMatchNone && !answer_en_.empty()) {
            add("en-US", answer_en_.c_str(), "model_echo");
        }
        if (teacher_out) {
            *teacher_out = last_feedback_;
        }
        return;
    }
    const bool is_ask = type && strcmp(type, "ask") == 0;
    const bool teach_flow_ok =
        (is_ask || !hide_answer) &&
        (is_ask || activity_type_.empty() || activity_type_ == "learn_vocab" ||
         activity_type_ == "repeat_phrase" || activity_type_ == "ask_answer" || reveal);
    cJSON* speech_out = nullptr;
    if (out) {
        speech_out = cJSON_AddArrayToObject(out, "speech");
    }

    auto add_seg = [&](const char* lang, const char* text, const char* role) {
        if (!text || !text[0]) {
            return;
        }
        const bool english = lang && strncmp(lang, "en", 2) == 0;
        std::string clean = english ? std::string(text) : SanitizePersona(text);
        if (clean.empty()) {
            return;
        }
        if (speech_out) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "lang", lang && lang[0] ? lang : "vi-VN");
            cJSON_AddStringToObject(item, "text", clean.c_str());
            if (role && role[0]) {
                cJSON_AddStringToObject(item, "role", role);
            }
            cJSON_AddItemToArray(speech_out, item);
        }
        if (role && strncmp(role, "model", 5) == 0) {
            if (built.find(clean) == std::string::npos) {
                if (!built.empty()) {
                    built += " ";
                }
                built += clean;
            }
        } else if (built.find(clean) == std::string::npos) {
            if (!built.empty()) {
                built += " ";
            }
            built += clean;
        }
    };

    auto keep_role = [&](const char* role) {
        if (!role || !role[0]) {
            return true;
        }
        const bool model = strncmp(role, "model", 5) == 0;
        if (hide_answer && model) {
            return false;
        }
        if (strcmp(role, "question") == 0 || strcmp(role, "ask") == 0 ||
            strcmp(role, "ask_ready") == 0) {
            return true;
        }
        if (strcmp(role, "hook") == 0) {
            return explain_level_ >= 2;
        }
        if (strcmp(role, "meaning") == 0) {
            return explain_level_ >= 1;
        }
        if (model || strcmp(role, "practice") == 0) {
            return true;
        }
        return explain_level_ >= 2;
    };

    bool used_flow = false;
    if (teach_flow_ok && cJSON_IsObject(card)) {
        cJSON* flow = cJSON_GetObjectItem(card, "teaching_flow");
        cJSON* arr = cJSON_IsObject(flow) ? cJSON_GetObjectItem(flow, "speech") : nullptr;
        if (cJSON_IsArray(arr)) {
            std::string prev;
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, arr) {
                if (!cJSON_IsObject(it)) {
                    continue;
                }
                const char* role = JsonStr(it, "role");
                if (!keep_role(role)) {
                    continue;
                }
                const char* lang = JsonStr(it, "lang");
                const char* text = JsonStr(it, "text");
                if (!text || !text[0]) {
                    continue;
                }
                const bool model = role && strncmp(role, "model", 5) == 0;
                if (!model && prev == text) {
                    continue;
                }
                prev = text;
                add_seg(lang && lang[0] ? lang : "vi-VN", text, role);
                used_flow = true;
            }
        }
    }

    if (!used_flow) {
        std::string fallback =
            TeacherForType(type, en, vi, teacher ? teacher : "", reveal);
        if (fallback.empty() && teacher) {
            fallback = teacher;
        }
        fallback = SanitizePersona(fallback);
        if (explain_level_ >= 1 && teach_flow_ok && cJSON_IsObject(card)) {
            cJSON* explain = cJSON_GetObjectItem(card, "explain");
            const char* want = (explain_level_ >= 2) ? "detailed" : "basic";
            cJSON* block = cJSON_IsObject(explain) ? cJSON_GetObjectItem(explain, want) : nullptr;
            if (!cJSON_IsObject(block) && explain_level_ >= 2 && cJSON_IsObject(explain)) {
                block = cJSON_GetObjectItem(explain, "basic");
            }
            cJSON* vi_e = cJSON_IsObject(block) ? cJSON_GetObjectItem(block, "vi") : nullptr;
            if (cJSON_IsString(vi_e) && vi_e->valuestring && vi_e->valuestring[0] &&
                fallback.find(vi_e->valuestring) == std::string::npos) {
                fallback = std::string(vi_e->valuestring) + " " + fallback;
            }
        }
        if (!fallback.empty()) {
            add_seg("vi-VN", fallback.c_str(), "teacher");
            if (!hide_answer && en && en[0] && fallback.find(en) == std::string::npos) {
                add_seg("en-US", en, "model_1");
            }
        }
        built = fallback;
        if (!hide_answer && en && en[0] && built.find(en) == std::string::npos) {
            if (!built.empty()) {
                built += " ";
            }
            built += en;
        }
    }

    if (is_ask && !reveal) {
        if (ask_q_.empty() && cJSON_IsObject(card)) {
            ask_q_ = ExtractAskQuestion(card);
        }
        if (!ask_q_.empty()) {
            add_seg("en-US", ask_q_.c_str(), "question");
            add_seg("en-US", ask_q_.c_str(), "question_2");
        }
    }

    if (WaitsForSpeech() && with_teacher && !reveal) {
        wait_for_speech_ = true;
        if (item_phase_ < 3) {
            item_phase_ = 3;
        }
    }

    if (teacher_out) {
        *teacher_out = built.empty() ? SanitizePersona(teacher ? teacher : "") : built;
    }
}

void SpeakLesson::AppendActivityFields(cJSON* out, bool with_secrets) {
    if (!out) {
        return;
    }
    cJSON_AddStringToObject(out, "activity", activity_id_.c_str());
    cJSON_AddStringToObject(out, "activity_type", activity_type_.c_str());
    cJSON_AddStringToObject(out, "image_field", image_field_.c_str());
    cJSON_AddBoolToObject(out, "locked", activity_locked_);
    cJSON_AddNumberToObject(out, "attempts", attempts_);
    const char* ph = "show";
    if (item_phase_ <= 1) {
        ph = "preview";
    } else if (item_phase_ == 3) {
        ph = "listen";
    } else if (item_phase_ == 4) {
        ph = "hint";
    } else if (item_phase_ >= 5) {
        ph = "reveal";
    }
    cJSON_AddStringToObject(out, "item_phase", ph);
    if (IsFlip()) {
        FlipAppendSnapshot(out);
    } else if (!playlist_.empty() && index_ >= 0 && index_ < static_cast<int>(playlist_.size())) {
        cJSON_AddStringToObject(out, "card_id", playlist_[index_].c_str());
    }
    cJSON* pl = cJSON_AddArrayToObject(out, "playlist");
    for (const auto& id : playlist_) {
        cJSON_AddItemToArray(pl, cJSON_CreateString(id.c_str()));
    }
    cJSON_AddStringToObject(out, "verdict", last_verdict_.c_str());
    cJSON_AddStringToObject(out, "hint", last_hint_.c_str());
    if (!last_feedback_.empty()) {
        cJSON_AddStringToObject(out, "feedback", last_feedback_.c_str());
    }
    cJSON_AddBoolToObject(out, "wait_for_speech", wait_for_speech_ && !item_ok_);
    if (feedback_pending_ && !feedback_spoken_ && !last_feedback_.empty()) {
        cJSON_AddStringToObject(out, "next_action", "speak_feedback");
    } else if (feedback_pending_ && feedback_spoken_ && advance_after_feedback_) {
        cJSON_AddStringToObject(out, "next_action", "next");
    } else if (wait_for_speech_ && !item_ok_ && !done_ask_) {
        cJSON_AddStringToObject(out, "next_action", "wait");
    }
    cJSON_AddBoolToObject(out, "reveal", last_reveal_);
    if (!hidden_id_.empty()) {
        cJSON_AddStringToObject(out, "hidden_id", hidden_id_.c_str());
    }
    if (!group_ids_.empty()) {
        cJSON* g = cJSON_AddArrayToObject(out, "group_ids");
        for (const auto& id : group_ids_) {
            cJSON_AddItemToArray(g, cJSON_CreateString(id.c_str()));
        }
    }
    auto& prog = SpeakProgress::Get();
    const uint8_t b = prog.ByteFor(unit_, CurrentActIndex());
    cJSON_AddStringToObject(out, "act_status", SpeakProgress::StatusName(SpeakProgress::StatusOf(b)).c_str());
    cJSON_AddBoolToObject(out, "act_mastered", SpeakProgress::MasteredOf(b));
    (void)with_secrets;
}

std::string SpeakLesson::SetAssessment(const std::string& mode) {
    LoadNvs();
    SpeakProgress::Get().SetAssessment(SpeakProgress::ParseAssess(mode));
    return StatusJson();
}

std::string SpeakLesson::SetStudent(int slot, const std::string& name) {
    LoadNvs();
    (void)slot;
    auto& prog = SpeakProgress::Get();
    if (!name.empty()) {
        prog.SetStudentName(0, name);
    }
    prog.SetSlot(0);
    return StatusJson();
}

std::string SpeakLesson::SetItemPhase(const std::string& phase, const std::string& card_id,
                                      const std::string& event_id) {
    LoadNvs();
    if (IsFlip()) {
        if (phase == "skip") {
            if (!active_card_id_.empty()) {
                FlipResolveCurrent(false, false, true);
            }
            ShowCurrent();
            return CurrentCardJson();
        }
        if (!card_id.empty() && (phase == "show" || phase == "select" || phase == "listen")) {
            return FlipSelect(card_id, event_id);
        }
        if (phase == "preview") {
            flip_phase_ = 1;
            item_phase_ = 1;
        }
        return CurrentCardJson();
    }
    if (!card_id.empty() && !playlist_.empty()) {
        for (size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_[i] == card_id) {
                index_ = static_cast<int>(i);
                LoadSentenceItem(index_);
                break;
            }
        }
    }
    if (phase == "preview") {
        item_phase_ = 1;
    } else if (phase == "listen") {
        item_phase_ = 3;
    } else if (phase == "hint") {
        item_phase_ = 4;
    } else if (phase == "reveal") {
        item_phase_ = 5;
        last_reveal_ = true;
    } else {
        item_phase_ = 2;
    }
    if (phase == "show" && !card_id.empty()) {
        ShowCurrent();
    }
    return CurrentCardJson();
}

std::string SpeakLesson::ProgressJson() const {
    return SpeakProgress::Get().SummaryJson();
}

std::string SpeakLesson::Evaluate(const std::string& text, const std::string& verdict,
                                  const std::string& source) {
    LoadNvs();
    if (just_auto_advanced_ && !item_ok_) {
        ESP_LOGI(TAG, "Evaluate ignored: waiting for new card intro");
        cJSON* hold = cJSON_CreateObject();
        cJSON_AddStringToObject(hold, "next_action", "wait");
        cJSON_AddBoolToObject(hold, "wait_for_speech", false);
        cJSON_AddStringToObject(hold, "verdict", "hold");
        char* printed = cJSON_PrintUnformatted(hold);
        std::string result = printed ? printed : "{}";
        cJSON_free(printed);
        cJSON_Delete(hold);
        return result;
    }
    if (item_ok_ && (last_correct_ || advance_after_feedback_)) {
        ESP_LOGI(TAG, "Evaluate ignored: already passed this card");
        cJSON* keep = cJSON_CreateObject();
        cJSON_AddStringToObject(keep, "next_action", "speak_feedback");
        cJSON_AddStringToObject(keep, "verdict", last_verdict_.c_str());
        cJSON_AddStringToObject(keep, "hint", last_hint_.c_str());
        if (!last_feedback_.empty()) {
            cJSON_AddStringToObject(keep, "feedback", last_feedback_.c_str());
        }
        char* printed = cJSON_PrintUnformatted(keep);
        std::string result = printed ? printed : "{}";
        cJSON_free(printed);
        cJSON_Delete(keep);
        return result;
    }
    xz_status_hits_ = 0;
    last_xz_status_us_ = 0;
    EnsureUnitLoaded();
    if (!activity_id_.empty()) {
        EnsureActivitiesLoaded();
        if (playlist_.empty()) {
            BuildPlaylist();
        }
    }
    LoadSentenceItem(index_);
    bool personal = allow_personal_ || activity_type_ == "ask_answer";
    if (!personal && !IsFlip() && !cached_json_.empty()) {
        cJSON* root = cJSON_Parse(cached_json_.c_str());
        cJSON* card = !playlist_.empty() && index_ < static_cast<int>(playlist_.size())
                          ? FindCardById(root, playlist_[index_].c_str())
                          : CardAt(root, index_);
        personal = strcmp(JsonStr(card, "type"), "ask") == 0;
        cJSON_Delete(root);
    }
    std::string v = verdict;
    for (char& c : v) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    cJSON* out = cJSON_CreateObject();
    auto arm_feedback = [&](bool advance) {
        feedback_pending_ = true;
        feedback_spoken_ = false;
        advance_after_feedback_ = advance;
        praise_i_ = static_cast<uint8_t>(praise_i_ + 1);
    };
    auto note_turn = [&](bool passed) {
        const std::string en = CurrentTeachEn();
        const bool phrase = en.find(' ') != std::string::npos ||
                            activity_type_.find("phrase") != std::string::npos ||
                            activity_type_.find("sentence") != std::string::npos ||
                            activity_type_.find("ask") != std::string::npos;
        SpeakProgress::Get().AddSpeakTurn(passed, phrase);
    };
    if (!text.empty() && text == last_heard_ && feedback_pending_ && !feedback_spoken_ &&
        !last_feedback_.empty()) {
        auto finish_dup = [&](const char* next_action) {
            const char* na = "speak_feedback";
            cJSON_AddStringToObject(out, "next_action", na);
            cJSON_AddStringToObject(out, "verdict", last_verdict_.c_str());
            cJSON_AddStringToObject(out, "hint", last_hint_.c_str());
            cJSON_AddStringToObject(out, "feedback", last_feedback_.c_str());
            cJSON_AddBoolToObject(out, "pronunciation_scored", false);
            char* printed = cJSON_PrintUnformatted(out);
            std::string result = printed ? printed : "{}";
            cJSON_free(printed);
            cJSON_Delete(out);
            return result;
        };
        return finish_dup("wait");
    }
    auto finish = [&](const char* next_action) {
        const char* na = (feedback_pending_ && !last_feedback_.empty()) ? "speak_feedback" : next_action;
        cJSON_AddStringToObject(out, "next_action", na);
        cJSON_AddStringToObject(out, "verdict", last_verdict_.c_str());
        cJSON_AddStringToObject(out, "hint", last_hint_.c_str());
        if (!last_feedback_.empty()) {
            cJSON_AddStringToObject(out, "feedback", last_feedback_.c_str());
        }
        if (!last_heard_.empty()) {
            cJSON_AddStringToObject(out, "heard_text", last_heard_.c_str());
        }
        const char* mk = "none";
        if (last_match_ == SpeakProgress::kMatchExact) {
            mk = "exact";
        } else if (last_match_ == SpeakProgress::kMatchNear) {
            mk = "near";
        }
        cJSON_AddStringToObject(out, "match", mk);
        cJSON_AddBoolToObject(out, "pronunciation_scored", false);
        cJSON_AddBoolToObject(out, "wait_for_speech", wait_for_speech_ && !item_ok_);
        cJSON_AddBoolToObject(out, "reveal", last_reveal_);
        if (last_reveal_ && !answer_en_.empty()) {
            cJSON_AddStringToObject(out, "answer_en", answer_en_.c_str());
        }
        cJSON_AddNumberToObject(out, "attempts", attempts_);
        cJSON_AddStringToObject(out, "activity", activity_id_.c_str());
        cJSON_AddStringToObject(out, "unit", unit_.c_str());
        cJSON_AddNumberToObject(out, "index", index_);
        if (IsFlip()) {
            cJSON_AddStringToObject(out, "selected_card_id", active_card_id_.c_str());
            cJSON_AddBoolToObject(out, "graded_by_playlist_index", false);
        }
        char* printed = cJSON_PrintUnformatted(out);
        std::string result = printed ? printed : "{}";
        cJSON_free(printed);
        cJSON_Delete(out);
        return result;
    };

    if (IsFlip()) {
        if (v == "skip") {
            last_verdict_ = "skipped";
            last_feedback_ = "Mình ghi cần ôn lại. Bạn chọn thẻ khác nhé.";
            last_hint_ = last_feedback_;
            last_reveal_ = false;
            FlipResolveCurrent(false, false, true);
            arm_feedback(false);
            return finish("wait");
        }
        if (flip_phase_ != 3 || active_card_id_.empty()) {
            last_verdict_ = "unclear";
            last_feedback_ = "Bạn chọn một thẻ trước nhé. Mình chưa chấm.";
            last_hint_ = last_feedback_;
            last_reveal_ = false;
            arm_feedback(false);
            return finish("wait");
        }
        answer_en_ = FlipAnswerForCard(active_card_id_);
    }

    last_heard_ = text;
    last_match_ = SpeakProgress::kMatchNone;

    if (v == "silence") {
        last_verdict_ = "silence";
        last_feedback_ = BuildFeedback("silence", text);
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        item_phase_ = 3;
        wait_for_speech_ = true;
        arm_feedback(false);
        return finish("wait");
    }

    if (v == "unclear" || v == "unknown") {
        last_verdict_ = "unclear";
        last_feedback_ = BuildFeedback("uncertain", text);
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        item_phase_ = 3;
        wait_for_speech_ = true;
        arm_feedback(false);
        return finish("wait");
    }

    const bool dummy_ok = (text.empty() || text == "ok" || text == "yes") &&
                          (v == "correct" || v == "yes" || v == "heard" || v.empty());
    if (dummy_ok) {
        last_verdict_ = "heard";
        last_feedback_ = BuildFeedback("heard", "");
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        item_phase_ = 3;
        wait_for_speech_ = true;
        arm_feedback(false);
        return finish("wait");
    }

    if (text.empty()) {
        last_verdict_ = "unclear";
        last_feedback_ = BuildFeedback("uncertain", "");
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        item_phase_ = 3;
        wait_for_speech_ = true;
        arm_feedback(false);
        return finish("wait");
    }

    std::string expect = answer_en_;
    std::string meaning_vi;
    if (!cached_json_.empty()) {
        cJSON* root = cJSON_Parse(cached_json_.c_str());
        cJSON* card = nullptr;
        if (IsFlip() && !active_card_id_.empty()) {
            card = FindCardById(root, active_card_id_.c_str());
        } else if (!playlist_.empty() && index_ < static_cast<int>(playlist_.size())) {
            card = FindCardById(root, playlist_[index_].c_str());
        } else {
            card = CardAt(root, index_);
        }
        if (expect.empty()) {
            expect = JsonStr(card, "en");
            if (answer_en_.empty()) {
                answer_en_ = expect;
            }
        }
        meaning_vi = JsonStr(card, "vi");
        cJSON_Delete(root);
    }
    if (expect.empty() && IsFlip()) {
        expect = FlipAnswerForCard(active_card_id_);
        answer_en_ = expect;
    }

    const auto kind = SpeakProgress::ClassifyMatch(text, expect, meaning_vi);
    last_match_ = static_cast<uint8_t>(kind);
    ESP_LOGI(TAG, "Match %s expect='%s' attempts=%u",
             kind == SpeakProgress::kMatchExact ? "exact"
                                                : (kind == SpeakProgress::kMatchNear ? "near" : "none"),
             expect.c_str(), static_cast<unsigned>(attempts_));

    if (kind == SpeakProgress::kMatchNone && personal && !text.empty()) {
        last_verdict_ = "accepted";
        last_feedback_ = "Cảm ơn bạn. Mình ghi nhận câu trả lời của bạn.";
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        last_correct_ = false;
        item_ok_ = true;
        wait_for_speech_ = false;
        items_correct_++;
        attempts_ = 0;
        SpeakProgress::Get().MarkOpened(unit_, CurrentActIndex(), static_cast<uint8_t>(index_));
        note_turn(true);
        arm_feedback(true);
        return finish("wait");
    }

    if (kind == SpeakProgress::kMatchExact) {
        last_verdict_ = "correct";
        last_correct_ = true;
        last_feedback_ = BuildFeedback(attempts_ > 0 ? "retry_success" : "correct", text);
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        item_ok_ = true;
        wait_for_speech_ = false;
        items_correct_++;
        const bool first_try = (attempts_ == 0);
        attempts_ = 0;
        SpeakProgress::Get().MarkOpened(unit_, CurrentActIndex(), static_cast<uint8_t>(index_));
        note_turn(true);
        arm_feedback(true);
        if (IsFlip()) {
            FlipResolveCurrent(true, first_try, false);
        }
        return finish("wait");
    }

    if (kind == SpeakProgress::kMatchNear) {
        last_verdict_ = "near";
        last_correct_ = false;
        last_feedback_ = BuildFeedback("near", text);
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        needs_review_act_ = true;
        item_ok_ = true;
        wait_for_speech_ = false;
        SpeakProgress::Get().MarkOpened(unit_, CurrentActIndex(), static_cast<uint8_t>(index_));
        note_turn(true);
        arm_feedback(true);
        if (IsFlip()) {
            FlipResolveCurrent(false, false, true);
        }
        return finish("wait");
    }

    items_wrong_++;
    attempts_++;
    last_verdict_ = "wrong";
    last_correct_ = false;
    if (attempts_ < 3) {
        last_feedback_ = BuildFeedback("incorrect", text);
        last_hint_ = last_feedback_;
        last_reveal_ = false;
        wait_for_speech_ = true;
        item_phase_ = 4;
        note_turn(false);
        arm_feedback(false);
        return finish("wait");
    }
    last_verdict_ = "pass_on";
    last_feedback_ = BuildFeedback("pass_on", text);
    last_hint_ = last_feedback_;
    last_reveal_ = true;
    needs_review_act_ = true;
    item_phase_ = 5;
    item_ok_ = true;
    wait_for_speech_ = false;
    note_turn(true);
    arm_feedback(true);
    if (IsFlip()) {
        FlipResolveCurrent(false, false, true);
    }
    return finish("wait");
}

bool SpeakLesson::IsFlip() const {
    return activity_type_ == "flip_guess";
}

void SpeakLesson::ResetFlipSession() {
    flip_phase_ = 0;
    flip_round_n_ = 0;
    flip_preview_i_ = 0;
    flip_review_round_ = false;
    flip_review_used_ = false;
    flip_any_review_ = false;
    flip_all_first_ok_ = true;
    flip_seed_ = 0;
    flip_deck_.clear();
    flip_round_.clear();
    flip_resolved_.clear();
    flip_review_.clear();
    active_card_id_.clear();
    flip_session_id_.clear();
    flip_round_id_.clear();
    last_flip_event_id_.clear();
}

void SpeakLesson::FlipShuffle(std::vector<std::string>* ids, uint32_t seed) {
    if (!ids || ids->size() < 2) {
        return;
    }
    uint32_t x = seed ? seed : 1;
    for (size_t i = ids->size() - 1; i > 0; --i) {
        x = x * 1664525u + 1013904223u;
        const size_t j = static_cast<size_t>(x % (i + 1));
        std::swap((*ids)[i], (*ids)[j]);
    }
}

int SpeakLesson::FlipDeckIndex(const std::string& id) const {
    for (size_t i = 0; i < flip_deck_.size(); ++i) {
        if (flip_deck_[i] == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SpeakLesson::FlipCardResolved(const std::string& id) const {
    return std::find(flip_resolved_.begin(), flip_resolved_.end(), id) != flip_resolved_.end();
}

bool SpeakLesson::FlipCardInRound(const std::string& id) const {
    return std::find(flip_round_.begin(), flip_round_.end(), id) != flip_round_.end();
}

std::string SpeakLesson::FlipFocusCardId() const {
    if (flip_phase_ == 3 && !active_card_id_.empty()) {
        return active_card_id_;
    }
    if (flip_phase_ == 1 && flip_preview_i_ < flip_deck_.size()) {
        return flip_deck_[flip_preview_i_];
    }
    if (!active_card_id_.empty()) {
        return active_card_id_;
    }
    return "";
}

std::string SpeakLesson::FlipAnswerForCard(const std::string& card_id) const {
    if (card_id.empty() || cached_json_.empty()) {
        return "";
    }
    cJSON* root = cJSON_Parse(cached_json_.c_str());
    cJSON* card = FindCardById(root, card_id.c_str());
    std::string en = JsonStr(card, "en");
    cJSON_Delete(root);
    return en;
}

void SpeakLesson::FlipSaveNvs() {
    nvs_handle_t nvs = 0;
    if (nvs_open(SPEAK_NVS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    uint8_t blob[48] = {};
    memcpy(blob, unit_.c_str(), std::min<size_t>(unit_.size(), 4));
    blob[4] = flip_phase_;
    blob[5] = flip_round_n_;
    blob[6] = flip_preview_i_;
    blob[7] = static_cast<uint8_t>(std::min<size_t>(flip_deck_.size(), 16));
    uint16_t resolved = 0;
    uint16_t review = 0;
    for (size_t i = 0; i < flip_deck_.size() && i < 16; ++i) {
        if (FlipCardResolved(flip_deck_[i])) {
            resolved = static_cast<uint16_t>(resolved | (1u << i));
        }
        if (std::find(flip_review_.begin(), flip_review_.end(), flip_deck_[i]) != flip_review_.end()) {
            review = static_cast<uint16_t>(review | (1u << i));
        }
    }
    memcpy(blob + 8, &resolved, 2);
    memcpy(blob + 10, &review, 2);
    blob[12] = static_cast<uint8_t>(std::min<size_t>(flip_round_.size(), 8));
    for (size_t i = 0; i < flip_round_.size() && i < 8; ++i) {
        const int ix = FlipDeckIndex(flip_round_[i]);
        blob[13 + i] = static_cast<uint8_t>(ix < 0 ? 0xFF : ix);
    }
    const int ax = FlipDeckIndex(active_card_id_);
    blob[21] = static_cast<uint8_t>(ax < 0 ? 0 : ax + 1);
    blob[22] = static_cast<uint8_t>((flip_review_used_ ? 1 : 0) | (flip_review_round_ ? 2 : 0) |
                                    (flip_any_review_ ? 4 : 0) | (flip_all_first_ok_ ? 8 : 0));
    memcpy(blob + 24, &flip_seed_, 4);
    nvs_set_blob(nvs, "flp", blob, sizeof(blob));
    nvs_set_str(nvs, "fls", flip_session_id_.c_str());
    nvs_set_str(nvs, "flr", flip_round_id_.c_str());
    nvs_commit(nvs);
    nvs_close(nvs);
}

bool SpeakLesson::FlipRestoreIfPossible() {
    nvs_handle_t nvs = 0;
    if (nvs_open(SPEAK_NVS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t blob[48] = {};
    size_t len = sizeof(blob);
    const bool ok = nvs_get_blob(nvs, "flp", blob, &len) == ESP_OK && len >= 28;
    char sess[20] = {};
    size_t slen = sizeof(sess);
    nvs_get_str(nvs, "fls", sess, &slen);
    char rid[20] = {};
    size_t rlen = sizeof(rid);
    nvs_get_str(nvs, "flr", rid, &rlen);
    nvs_close(nvs);
    if (!ok) {
        return false;
    }
    if (memcmp(blob, unit_.c_str(), std::min<size_t>(unit_.size(), 4)) != 0) {
        return false;
    }
    if (blob[4] == 0 || blob[4] >= 4) {
        return false;
    }
    flip_phase_ = blob[4];
    flip_round_n_ = blob[5];
    flip_preview_i_ = blob[6];
    uint16_t resolved = 0;
    uint16_t review = 0;
    memcpy(&resolved, blob + 8, 2);
    memcpy(&review, blob + 10, 2);
    flip_resolved_.clear();
    flip_review_.clear();
    for (size_t i = 0; i < flip_deck_.size() && i < 16; ++i) {
        if (resolved & (1u << i)) {
            flip_resolved_.push_back(flip_deck_[i]);
        }
        if (review & (1u << i)) {
            flip_review_.push_back(flip_deck_[i]);
        }
    }
    flip_round_.clear();
    const uint8_t rn = blob[12];
    for (uint8_t i = 0; i < rn && i < 8; ++i) {
        const uint8_t ix = blob[13 + i];
        if (ix < flip_deck_.size()) {
            flip_round_.push_back(flip_deck_[ix]);
        }
    }
    active_card_id_.clear();
    if (blob[21] > 0 && blob[21] <= flip_deck_.size()) {
        active_card_id_ = flip_deck_[blob[21] - 1];
    }
    flip_review_used_ = (blob[22] & 1) != 0;
    flip_review_round_ = (blob[22] & 2) != 0;
    flip_any_review_ = (blob[22] & 4) != 0;
    flip_all_first_ok_ = (blob[22] & 8) != 0;
    memcpy(&flip_seed_, blob + 24, 4);
    flip_session_id_ = sess;
    flip_round_id_ = rid;
    index_ = static_cast<int>(flip_resolved_.size());
    if (flip_phase_ == 1) {
        item_phase_ = 1;
    } else if (flip_phase_ == 3) {
        item_phase_ = 3;
    } else {
        item_phase_ = 2;
    }
    ESP_LOGI(TAG, "Flip restore %s phase=%u resolved=%u", unit_.c_str(), flip_phase_,
             (unsigned)flip_resolved_.size());
    return flip_phase_ != 0 && !flip_deck_.empty();
}

void SpeakLesson::FlipInitFromActivity() {
    flip_deck_.clear();
    cJSON* cards_root = cached_json_.empty() ? nullptr : cJSON_Parse(cached_json_.c_str());
    for (const auto& id : playlist_) {
        cJSON* card = FindCardById(cards_root, id.c_str());
        const char* game = JsonStr(card, "game_image");
        if (game && game[0]) {
            flip_deck_.push_back(id);
        }
    }
    cJSON_Delete(cards_root);
    if (flip_deck_.empty()) {
        flip_deck_ = playlist_;
        scoring_graded_ = false;
    }
    if (FlipRestoreIfPossible()) {
        return;
    }
    flip_phase_ = 1;
    flip_round_n_ = 0;
    flip_preview_i_ = 0;
    flip_review_round_ = false;
    flip_review_used_ = false;
    flip_any_review_ = false;
    flip_all_first_ok_ = true;
    flip_resolved_.clear();
    flip_review_.clear();
    flip_round_.clear();
    active_card_id_.clear();
    last_flip_event_id_.clear();
    flip_seed_ = esp_random();
    char sess[12];
    snprintf(sess, sizeof(sess), "%08lx", static_cast<unsigned long>(flip_seed_));
    flip_session_id_ = sess;
    flip_round_id_.clear();
    item_phase_ = 1;
    index_ = 0;
    FlipSaveNvs();
}

void SpeakLesson::FlipSetupRound() {
    std::vector<std::string> remain;
    if (flip_review_round_) {
        for (const auto& id : flip_review_) {
            if (!FlipCardResolved(id)) {
                remain.push_back(id);
            }
        }
    } else {
        for (const auto& id : flip_deck_) {
            if (!FlipCardResolved(id)) {
                remain.push_back(id);
            }
        }
    }
    if (remain.empty()) {
        if (!flip_review_.empty() && !flip_review_used_) {
            flip_review_round_ = true;
            flip_review_used_ = true;
            FlipSetupRound();
            return;
        }
        flip_phase_ = 4;
        done_ask_ = true;
        teach_armed_ = true;
        needs_review_act_ = flip_any_review_;
        SpeakProgress::Get().MarkItem(unit_, CurrentActIndex(),
                                      static_cast<uint8_t>(flip_resolved_.size()),
                                      true, true, flip_any_review_,
                                      flip_all_first_ok_ && !flip_any_review_);
        FlipSaveNvs();
        return;
    }
    const int want = (flip_review_round_ || flip_round_n_ > 0) ? 4 : 2;
    if (static_cast<int>(remain.size()) < want) {
        flip_round_ = remain;
    } else {
        flip_round_.assign(remain.begin(), remain.begin() + want);
    }
    flip_seed_ = esp_random();
    FlipShuffle(&flip_round_, flip_seed_);
    char rid[16];
    snprintf(rid, sizeof(rid), "r%u-%08lx", flip_round_n_, static_cast<unsigned long>(flip_seed_));
    flip_round_id_ = rid;
    flip_round_n_++;
    active_card_id_.clear();
    last_flip_event_id_.clear();
    attempts_ = 0;
    last_reveal_ = false;
    flip_phase_ = 2;
    item_phase_ = 2;
    FlipSaveNvs();
}

void SpeakLesson::FlipAdvancePreview() {
    if (flip_phase_ != 1 || flip_deck_.empty()) {
        return;
    }
    if (flip_preview_i_ + 1 < flip_deck_.size()) {
        flip_preview_i_++;
        item_phase_ = 1;
        FlipSaveNvs();
        return;
    }
    FlipSetupRound();
}

void SpeakLesson::FlipResolveCurrent(bool correct, bool first_try, bool needs_review) {
    if (active_card_id_.empty()) {
        return;
    }
    const std::string id = active_card_id_;
    if (!FlipCardResolved(id)) {
        flip_resolved_.push_back(id);
    }
    if (!correct || needs_review) {
        flip_any_review_ = true;
        flip_all_first_ok_ = false;
        if (std::find(flip_review_.begin(), flip_review_.end(), id) == flip_review_.end()) {
            flip_review_.push_back(id);
        }
    } else if (!first_try) {
        flip_all_first_ok_ = false;
    }
    index_ = static_cast<int>(flip_resolved_.size());
    attempts_ = 0;
    active_card_id_.clear();
    last_flip_event_id_.clear();
    SpeakProgress::Get().MarkOpened(unit_, CurrentActIndex(), static_cast<uint8_t>(index_));
    bool round_left = false;
    for (const auto& cid : flip_round_) {
        if (!FlipCardResolved(cid)) {
            round_left = true;
            break;
        }
    }
    if (round_left) {
        flip_phase_ = 2;
        item_phase_ = 2;
        last_reveal_ = false;
        FlipSaveNvs();
        return;
    }
    FlipSetupRound();
}

void SpeakLesson::FlipAppendSnapshot(cJSON* out) const {
    if (!out) {
        return;
    }
    cJSON_AddBoolToObject(out, "is_matching_pairs_game", false);
    cJSON_AddStringToObject(out, "game_mode", "flip_one_card_then_say");
    const char* st = "intro";
    if (flip_phase_ == 1) {
        st = "preview";
    } else if (flip_phase_ == 2) {
        st = flip_review_round_ ? "review_round" : "await_select";
    } else if (flip_phase_ == 3) {
        st = "await_answer";
    } else if (flip_phase_ == 4) {
        st = "complete";
    }
    cJSON_AddStringToObject(out, "flip_state", st);
    cJSON_AddStringToObject(out, "session_id", flip_session_id_.c_str());
    cJSON_AddStringToObject(out, "round_id", flip_round_id_.c_str());
    const std::string focus = FlipFocusCardId();
    cJSON_AddStringToObject(out, "card_id", focus.c_str());
    cJSON_AddStringToObject(out, "active_card_id", active_card_id_.c_str());
    if (flip_phase_ == 1 && flip_preview_i_ < flip_deck_.size()) {
        cJSON_AddStringToObject(out, "preview_card_id", flip_deck_[flip_preview_i_].c_str());
    }
    cJSON* round = cJSON_AddArrayToObject(out, "round_cards");
    if (flip_phase_ == 1) {
        for (const auto& id : flip_deck_) {
            cJSON_AddItemToArray(round, cJSON_CreateString(id.c_str()));
        }
    } else {
        for (const auto& id : flip_round_) {
            cJSON_AddItemToArray(round, cJSON_CreateString(id.c_str()));
        }
    }
    cJSON* resolved = cJSON_AddArrayToObject(out, "resolved_ids");
    for (const auto& id : flip_resolved_) {
        cJSON_AddItemToArray(resolved, cJSON_CreateString(id.c_str()));
    }
    cJSON* review = cJSON_AddArrayToObject(out, "review_ids");
    for (const auto& id : flip_review_) {
        cJSON_AddItemToArray(review, cJSON_CreateString(id.c_str()));
    }
    cJSON_AddNumberToObject(out, "resolved_count", static_cast<int>(flip_resolved_.size()));
    cJSON_AddNumberToObject(out, "total_unique", static_cast<int>(flip_deck_.size()));
    cJSON_AddBoolToObject(out, "requires_real_stt", scoring_graded_);
    cJSON_AddBoolToObject(out, "onboard_stt", false);
    cJSON_AddStringToObject(out, "speech_source", "self.speak.heard_or_typed_transcript");
    cJSON_AddBoolToObject(out, "graded_by_playlist_index", false);
}

std::string SpeakLesson::FlipSelect(const std::string& card_id, const std::string& event_id,
                                   const std::string& session_id, const std::string& round_id) {
    LoadNvs();
    if (!IsFlip()) {
        return SetItemPhase("show", card_id, event_id);
    }
    if (!event_id.empty() && event_id == last_flip_event_id_ && active_card_id_ == card_id) {
        return CurrentCardJson();
    }
    if (flip_phase_ == 3 && !active_card_id_.empty()) {
        return CurrentCardJson();
    }
    if (flip_phase_ == 1) {
        return CurrentCardJson();
    }
    if (!session_id.empty() && !flip_session_id_.empty() && session_id != flip_session_id_) {
        return CurrentCardJson();
    }
    if (!round_id.empty() && !flip_round_id_.empty() && round_id != flip_round_id_) {
        return CurrentCardJson();
    }
    if (card_id.empty() || !FlipCardInRound(card_id) || FlipCardResolved(card_id)) {
        last_hint_ = "Bạn chọn một thẻ đang úp trong lượt này nhé.";
        return CurrentCardJson();
    }
    active_card_id_ = card_id;
    last_flip_event_id_ = event_id;
    flip_phase_ = 3;
    item_phase_ = 3;
    attempts_ = 0;
    last_reveal_ = false;
    last_verdict_.clear();
    last_hint_.clear();
    answer_en_ = FlipAnswerForCard(active_card_id_);
    FlipSaveNvs();
    ShowCurrent();
    return CurrentCardJson();
}

std::string OttoSpeakDirectAiPrompt() {
    return SpeakLesson::Get().DirectAiExtraPrompt();
}

std::vector<std::string> OttoSpeakTeachSttCodes() {
    return SpeakLesson::Get().TeachSttLanguageCodes();
}

std::vector<std::string> OttoSpeakTeachSttVocab() {
    return SpeakLesson::Get().TeachSttVocabulary();
}

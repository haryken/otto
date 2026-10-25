#include "speak_progress.h"

#include <cJSON.h>
#include <nvs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define SPEAK_PROG_NVS "speak"

namespace {

constexpr const char* kActIds[SpeakProgress::kMaxActs] = {
    "learn-words",
    "repeat-phrases",
    "picture-naming",
    "complete-sentences",
    "flip-and-say",
    "missing-image",
    "answer-questions",
    "unit-review"
};

char HexNibble(uint8_t v) {
    return (v < 10) ? static_cast<char>('0' + v) : static_cast<char>('a' + (v - 10));
}

}  // namespace

SpeakProgress& SpeakProgress::Get() {
    static SpeakProgress inst;
    inst.Load();
    return inst;
}

int SpeakProgress::UnitIndex(const std::string& unit) {
    if (unit.size() < 2) {
        return -1;
    }
    int n = 0;
    const char* p = unit.c_str();
    if (*p == 'u' || *p == 'U') {
        ++p;
    }
    n = atoi(p);
    if (n < 1 || n > kMaxUnits) {
        return -1;
    }
    return n - 1;
}

int SpeakProgress::KnownActIndex(const std::string& activity_id) {
    if (activity_id == "flip-pairs" || activity_id == "flip-and-say") {
        return 4;
    }
    for (int i = 0; i < kMaxActs; ++i) {
        if (activity_id == kActIds[i]) {
            return i;
        }
    }
    return -1;
}

const char* SpeakProgress::KnownActId(int act) {
    if (act < 0 || act >= kMaxActs) {
        return "";
    }
    return kActIds[act];
}

uint8_t SpeakProgress::Pack(uint8_t status, bool mastered, uint8_t item) {
    return static_cast<uint8_t>((status & 3u) | (mastered ? 4u : 0u) | ((item & 31u) << 3));
}

uint8_t SpeakProgress::StatusOf(uint8_t b) {
    return static_cast<uint8_t>(b & 3u);
}

bool SpeakProgress::MasteredOf(uint8_t b) {
    return (b & 4u) != 0;
}

uint8_t SpeakProgress::ItemOf(uint8_t b) {
    return static_cast<uint8_t>((b >> 3) & 31u);
}

std::string SpeakProgress::StatusName(uint8_t status) {
    if (status == kInProgress) {
        return "in_progress";
    }
    if (status == kCompleted) {
        return "completed";
    }
    if (status == kNeedsReview) {
        return "needs_review";
    }
    return "not_started";
}

const char* SpeakProgress::AssessName(uint8_t mode) {
    if (mode == kEncourage) {
        return "encourage";
    }
    if (mode == kChallenge) {
        return "challenge";
    }
    return "guided";
}

uint8_t SpeakProgress::ParseAssess(const std::string& name) {
    if (name == "encourage") {
        return kEncourage;
    }
    if (name == "challenge") {
        return kChallenge;
    }
    return kGuided;
}

std::string SpeakProgress::NormalizeText(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool space = false;
    for (unsigned char c : in) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        if (c == '.' || c == '!' || c == '?' || c == ',' || c == '\'' || c == '"' || c == ':') {
            continue;
        }
        if (isspace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) {
            out.push_back(' ');
        }
        space = false;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

int SpeakProgress::EditDistance(const std::string& a, const std::string& b) {
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) {
        return static_cast<int>(m);
    }
    if (m == 0) {
        return static_cast<int>(n);
    }
    if (n > 24 || m > 24) {
        return static_cast<int>(n > m ? n : m);
    }
    int prev[25];
    int cur[25];
    for (size_t j = 0; j <= m; ++j) {
        prev[j] = static_cast<int>(j);
    }
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            const int del = prev[j] + 1;
            const int ins = cur[j - 1] + 1;
            const int sub = prev[j - 1] + cost;
            cur[j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
        }
        memcpy(prev, cur, (m + 1) * sizeof(int));
    }
    return prev[m];
}

std::string SpeakProgress::FoldVietnamese(const std::string& in) {
    std::string s = in;
    auto rep = [&](const char* from, const char* to) {
        size_t pos = 0;
        const size_t n = strlen(from);
        const size_t m = strlen(to);
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, n, to);
            pos += m;
        }
    };
    const char* marks[][2] = {
        {"à", "a"}, {"á", "a"}, {"ạ", "a"}, {"ả", "a"}, {"ã", "a"},
        {"ă", "a"}, {"ằ", "a"}, {"ắ", "a"}, {"ặ", "a"}, {"ẳ", "a"}, {"ẵ", "a"},
        {"â", "a"}, {"ầ", "a"}, {"ấ", "a"}, {"ậ", "a"}, {"ẩ", "a"}, {"ẫ", "a"},
        {"è", "e"}, {"é", "e"}, {"ẹ", "e"}, {"ẻ", "e"}, {"ẽ", "e"},
        {"ê", "e"}, {"ề", "e"}, {"ế", "e"}, {"ệ", "e"}, {"ể", "e"}, {"ễ", "e"},
        {"ì", "i"}, {"í", "i"}, {"ị", "i"}, {"ỉ", "i"}, {"ĩ", "i"},
        {"ò", "o"}, {"ó", "o"}, {"ọ", "o"}, {"ỏ", "o"}, {"õ", "o"},
        {"ô", "o"}, {"ồ", "o"}, {"ố", "o"}, {"ộ", "o"}, {"ổ", "o"}, {"ỗ", "o"},
        {"ơ", "o"}, {"ờ", "o"}, {"ớ", "o"}, {"ợ", "o"}, {"ở", "o"}, {"ỡ", "o"},
        {"ù", "u"}, {"ú", "u"}, {"ụ", "u"}, {"ủ", "u"}, {"ũ", "u"},
        {"ư", "u"}, {"ừ", "u"}, {"ứ", "u"}, {"ự", "u"}, {"ử", "u"}, {"ữ", "u"},
        {"ỳ", "y"}, {"ý", "y"}, {"ỵ", "y"}, {"ỷ", "y"}, {"ỹ", "y"},
        {"đ", "d"}, {"Đ", "d"},
        {"À", "a"}, {"Á", "a"}, {"Ạ", "a"}, {"Ả", "a"}, {"Ã", "a"},
        {"Ă", "a"}, {"Ằ", "a"}, {"Ắ", "a"}, {"Ặ", "a"}, {"Ẳ", "a"}, {"Ẵ", "a"},
        {"Â", "a"}, {"Ầ", "a"}, {"Ấ", "a"}, {"Ậ", "a"}, {"Ẩ", "a"}, {"Ẫ", "a"},
        {"È", "e"}, {"É", "e"}, {"Ẹ", "e"}, {"Ẻ", "e"}, {"Ẽ", "e"},
        {"Ê", "e"}, {"Ề", "e"}, {"Ế", "e"}, {"Ệ", "e"}, {"Ể", "e"}, {"Ễ", "e"},
        {"Ì", "i"}, {"Í", "i"}, {"Ị", "i"}, {"Ỉ", "i"}, {"Ĩ", "i"},
        {"Ò", "o"}, {"Ó", "o"}, {"Ọ", "o"}, {"Ỏ", "o"}, {"Õ", "o"},
        {"Ô", "o"}, {"Ồ", "o"}, {"Ố", "o"}, {"Ộ", "o"}, {"Ổ", "o"}, {"Ỗ", "o"},
        {"Ơ", "o"}, {"Ờ", "o"}, {"Ớ", "o"}, {"Ợ", "o"}, {"Ở", "o"}, {"Ỡ", "o"},
        {"Ù", "u"}, {"Ú", "u"}, {"Ụ", "u"}, {"Ủ", "u"}, {"Ũ", "u"},
        {"Ư", "u"}, {"Ừ", "u"}, {"Ứ", "u"}, {"Ự", "u"}, {"Ử", "u"}, {"Ữ", "u"},
        {"Ỳ", "y"}, {"Ý", "y"}, {"Ỵ", "y"}, {"Ỷ", "y"}, {"Ỹ", "y"},
    };
    for (const auto& pair : marks) {
        rep(pair[0], pair[1]);
    }
    return NormalizeText(s);
}

static bool HasWord(const std::string& hay, const std::string& w) {
    if (w.empty()) {
        return false;
    }
    size_t pos = 0;
    while ((pos = hay.find(w, pos)) != std::string::npos) {
        const bool left = (pos == 0) || hay[pos - 1] == ' ';
        const size_t end = pos + w.size();
        const bool right = (end >= hay.size()) || hay[end] == ' ';
        if (left && right) {
            return true;
        }
        pos = end;
    }
    return false;
}

static bool IsViStop(const std::string& t) {
    return t == "day" || t == "la" || t == "mot" || t == "cua" || t == "va" || t == "cai" ||
           t == "nay" || t == "kia" || t == "toi" || t == "minh" || t == "ban" || t == "con" ||
           t == "be" || t == "nha" || t == "noi" || t == "di" || t == "da" || t == "de" ||
           t == "ma" || t == "thi" || t == "se" || t == "cho" || t == "voi" || t == "cung";
}

static bool LooksProfane(const std::string& folded) {
    return HasWord(folded, "cac") || HasWord(folded, "deo") || HasWord(folded, "dit") ||
           folded.find("concac") != std::string::npos;
}

static bool MeaningNear(const std::string& heard, const std::string& meaning_vi) {
    const std::string a = SpeakProgress::FoldVietnamese(heard);
    const std::string b = SpeakProgress::FoldVietnamese(meaning_vi);
    if (a.size() < 2 || b.size() < 2 || LooksProfane(a)) {
        return false;
    }
    if (a == "ok" || a == "yes" || a == "con" || a == "ban") {
        return false;
    }
    if (a == b || a.find(b) != std::string::npos) {
        return true;
    }
    int need = 0;
    int hits = 0;
    std::string tok;
    for (size_t i = 0; i <= b.size(); ++i) {
        const char c = (i < b.size()) ? b[i] : ' ';
        if (c == ' ' || c == '/' || c == ',' || c == '-' || c == ';') {
            if (tok.size() >= 3 && !IsViStop(tok)) {
                need++;
                if (HasWord(a, tok) || (a == tok) ||
                    (a.size() <= 6 && SpeakProgress::EditDistance(a, tok) <= 1)) {
                    hits++;
                }
            } else if (tok.size() >= 2 && tok.size() < 3 && a == tok) {
                return true;
            }
            tok.clear();
        } else {
            tok.push_back(c);
        }
    }
    if (need == 0) {
        return a.size() <= 6 && SpeakProgress::EditDistance(a, b) <= 1;
    }
    return hits > 0 && hits * 2 >= need;
}

static void ReplaceIn(std::string& s, const char* from, const char* to) {
    size_t pos = 0;
    const size_t n = strlen(from);
    const size_t m = strlen(to);
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, n, to);
        pos += m;
    }
}

static std::string CompactLetters(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (unsigned char c : in) {
        if (c != ' ' && c != '-' && c != '\'') {
            s.push_back(static_cast<char>(c));
        }
    }
    return s;
}

static std::string CollapseDoubles(std::string s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (o.empty() || o.back() != c) {
            o.push_back(c);
        }
    }
    return o;
}

static std::string EnglishToViSound(const std::string& en) {
    std::string s = SpeakProgress::NormalizeText(en);
    ReplaceIn(s, "tion", "shon");
    ReplaceIn(s, "ther", "tho");
    ReplaceIn(s, "ough", "o");
    ReplaceIn(s, "ph", "f");
    ReplaceIn(s, "ck", "c");
    ReplaceIn(s, "cle", "co");
    ReplaceIn(s, "kn", "n");
    ReplaceIn(s, "wr", "r");
    ReplaceIn(s, "er", "o");
    ReplaceIn(s, "le", "o");
    return CollapseDoubles(CompactLetters(s));
}

static bool ViAccentNear(const std::string& heard, const std::string& expected) {
    const std::string ha = CompactLetters(SpeakProgress::FoldVietnamese(heard));
    const std::string vb = EnglishToViSound(expected);
    if (ha.size() < 3 || vb.size() < 3) {
        return false;
    }
    if (ha == "ok" || ha == "yes" || ha == "con" || ha == "concac" || ha == "ban") {
        return false;
    }
    if (ha == vb || SpeakProgress::EditDistance(ha, vb) <= 2) {
        return true;
    }
    return false;
}

SpeakProgress::MatchKind SpeakProgress::ClassifyMatch(const std::string& heard,
                                                      const std::string& expected,
                                                      const std::string& meaning_vi) {
    const std::string a = NormalizeText(heard);
    const std::string b = NormalizeText(expected);
    if (a.empty()) {
        return kMatchNone;
    }
    if (LooksProfane(FoldVietnamese(heard))) {
        return kMatchNone;
    }
    if (a == "ok" || a == "yes" || a == "dung" || a == "dung roi") {
        return kMatchNone;
    }
    if (!b.empty()) {
        if (a == b) {
            return kMatchExact;
        }
        auto last_word = [](const std::string& s) {
            const auto pos = s.find_last_of(' ');
            return pos == std::string::npos ? s : s.substr(pos + 1);
        };
        const std::string aw = last_word(a);
        const std::string bw = last_word(b);
        if (!bw.empty() && (a == bw || aw == bw)) {
            return kMatchExact;
        }
        if (a.find(b) != std::string::npos) {
            return kMatchExact;
        }
        if (b.find(a) != std::string::npos && a.size() >= 3) {
            return kMatchNear;
        }
        const int d = EditDistance(a, b);
        const int dw = (!aw.empty() && !bw.empty()) ? EditDistance(aw, bw) : d;
        const size_t n = a.size() > b.size() ? a.size() : b.size();
        if (dw <= 1 && !bw.empty() && bw.size() >= 3) {
            return kMatchNear;
        }
        if (n <= 4 && d <= 1) {
            return kMatchNear;
        }
        if (n <= 10 && d <= 2) {
            return kMatchNear;
        }
        const std::string fa = FoldVietnamese(heard);
        const std::string fb = FoldVietnamese(expected);
        if (fa.size() >= 3 && fb.size() >= 3 && EditDistance(fa, fb) <= 2) {
            return kMatchNear;
        }
        if (ViAccentNear(heard, expected)) {
            return kMatchNear;
        }
    }
    if (!meaning_vi.empty() && MeaningNear(heard, meaning_vi)) {
        return kMatchNear;
    }
    return kMatchNone;
}

bool SpeakProgress::AnswersMatch(const std::string& heard, const std::string& expected) {
    return ClassifyMatch(heard, expected) != kMatchNone;
}

void SpeakProgress::Load() {
    if (loaded_) {
        return;
    }
    nvs_handle_t nvs = 0;
    if (nvs_open(SPEAK_PROG_NVS, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t sid = 0;
        if (nvs_get_u8(nvs, "sid", &sid) == ESP_OK && sid < kMaxStudents) {
            slot_ = 0;
        }
        uint8_t asm_v = kGuided;
        if (nvs_get_u8(nvs, "asm", &asm_v) == ESP_OK && asm_v <= kChallenge) {
            assess_ = asm_v;
        }
        for (int s = 0; s < kMaxStudents; ++s) {
            char key[4] = {'p', static_cast<char>('0' + s), 0};
            size_t len = kBlobSize;
            if (nvs_get_blob(nvs, key, blob_[s], &len) != ESP_OK || len != kBlobSize) {
                memset(blob_[s], 0, kBlobSize);
            }
            char nk[4] = {'s', 'n', static_cast<char>('0' + s), 0};
            size_t nlen = 0;
            if (nvs_get_str(nvs, nk, nullptr, &nlen) == ESP_OK && nlen > 1 && nlen < 40) {
                names_[s].assign(nlen, '\0');
                nvs_get_str(nvs, nk, names_[s].data(), &nlen);
                if (!names_[s].empty() && names_[s].back() == '\0') {
                    names_[s].pop_back();
                }
            }
        }
        size_t slen = 42;
        uint8_t raw[42] = {};
        if (nvs_get_blob(nvs, "stat", raw, &slen) == ESP_OK && slen >= 42) {
            memcpy(&life_turns_, raw, 4);
            memcpy(&life_words_, raw + 4, 4);
            memcpy(&life_phrases_, raw + 8, 4);
            memcpy(&life_seconds_, raw + 12, 4);
            memcpy(&stats_day_, raw + 16, 2);
            memcpy(&today_turns_, raw + 18, 2);
            memcpy(&today_words_, raw + 20, 2);
            memcpy(&today_phrases_, raw + 22, 2);
            memcpy(&today_seconds_, raw + 24, 2);
            memcpy(&week_id_, raw + 26, 2);
            memcpy(week_sec_, raw + 28, 14);
        }
        nvs_close(nvs);
    }
    loaded_ = true;
    RollStatsDay();
}

void SpeakProgress::Save() {
    nvs_handle_t nvs = 0;
    if (nvs_open(SPEAK_PROG_NVS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_u8(nvs, "sid", slot_);
    nvs_set_u8(nvs, "asm", assess_);
    for (int s = 0; s < kMaxStudents; ++s) {
        char key[4] = {'p', static_cast<char>('0' + s), 0};
        nvs_set_blob(nvs, key, blob_[s], kBlobSize);
        char nk[4] = {'s', 'n', static_cast<char>('0' + s), 0};
        nvs_set_str(nvs, nk, names_[s].c_str());
    }
    uint8_t raw[42] = {};
    memcpy(raw, &life_turns_, 4);
    memcpy(raw + 4, &life_words_, 4);
    memcpy(raw + 8, &life_phrases_, 4);
    memcpy(raw + 12, &life_seconds_, 4);
    memcpy(raw + 16, &stats_day_, 2);
    memcpy(raw + 18, &today_turns_, 2);
    memcpy(raw + 20, &today_words_, 2);
    memcpy(raw + 22, &today_phrases_, 2);
    memcpy(raw + 24, &today_seconds_, 2);
    memcpy(raw + 26, &week_id_, 2);
    memcpy(raw + 28, week_sec_, 14);
    nvs_set_blob(nvs, "stat", raw, sizeof(raw));
    nvs_commit(nvs);
    nvs_close(nvs);
}

uint8_t* SpeakProgress::Blob() {
    return blob_[slot_];
}

const uint8_t* SpeakProgress::Blob() const {
    return blob_[slot_];
}

void SpeakProgress::SetSlot(int slot) {
    Load();
    if (slot < 0) {
        slot = 0;
    }
    if (slot >= kMaxStudents) {
        slot = kMaxStudents - 1;
    }
    if (slot_ == static_cast<uint8_t>(slot)) {
        return;
    }
    slot_ = static_cast<uint8_t>(slot);
    Save();
}

std::string SpeakProgress::StudentName(int slot) const {
    if (slot < 0 || slot >= kMaxStudents) {
        return "";
    }
    return names_[slot];
}

void SpeakProgress::SetStudentName(int slot, const std::string& name) {
    Load();
    if (slot < 0 || slot >= kMaxStudents) {
        return;
    }
    names_[slot] = name.substr(0, 24);
    Save();
}

void SpeakProgress::SetAssessment(uint8_t mode) {
    Load();
    if (mode > kChallenge) {
        mode = kGuided;
    }
    assess_ = mode;
    Save();
}

uint8_t SpeakProgress::ByteAt(int unit_i, int act_i) const {
    if (unit_i < 0 || unit_i >= kMaxUnits || act_i < 0 || act_i >= kMaxActs) {
        return 0;
    }
    return Blob()[unit_i * kMaxActs + act_i];
}

void SpeakProgress::SetByte(int unit_i, int act_i, uint8_t value, bool save) {
    Load();
    if (unit_i < 0 || unit_i >= kMaxUnits || act_i < 0 || act_i >= kMaxActs) {
        return;
    }
    Blob()[unit_i * kMaxActs + act_i] = value;
    if (save) {
        Save();
    }
}

uint8_t SpeakProgress::ByteFor(const std::string& unit, int act_i) const {
    return ByteAt(UnitIndex(unit), act_i);
}

void SpeakProgress::MarkOpened(const std::string& unit, int act_i, uint8_t item) {
    const int ui = UnitIndex(unit);
    if (ui < 0 || act_i < 0 || act_i >= kMaxActs) {
        return;
    }
    uint8_t b = ByteAt(ui, act_i);
    const uint8_t st = StatusOf(b);
    if (st == kCompleted || st == kNeedsReview) {
        SetByte(ui, act_i, Pack(st, MasteredOf(b), item));
        return;
    }
    SetByte(ui, act_i, Pack(kInProgress, false, item));
}

void SpeakProgress::MarkItem(const std::string& unit, int act_i, uint8_t item,
                             bool correct, bool finished, bool needs_review, bool mastered) {
    const int ui = UnitIndex(unit);
    if (ui < 0 || act_i < 0 || act_i >= kMaxActs) {
        return;
    }
    uint8_t st = kInProgress;
    if (finished) {
        st = needs_review ? kNeedsReview : kCompleted;
    }
    (void)correct;
    SetByte(ui, act_i, Pack(st, mastered && finished && !needs_review, item));
}

bool SpeakProgress::AllCompleted(const std::string& unit, const std::vector<int>& act_indices) const {
    const int ui = UnitIndex(unit);
    if (ui < 0) {
        return false;
    }
    if (act_indices.empty()) {
        return false;
    }
    for (int act_i : act_indices) {
        const uint8_t st = StatusOf(ByteAt(ui, act_i));
        if (st != kCompleted && st != kNeedsReview) {
            return false;
        }
    }
    return true;
}

std::string SpeakProgress::HexBlob() const {
    std::string hex;
    hex.resize(kBlobSize * 2);
    const uint8_t* p = Blob();
    for (int i = 0; i < kBlobSize; ++i) {
        hex[i * 2] = HexNibble(static_cast<uint8_t>(p[i] >> 4));
        hex[i * 2 + 1] = HexNibble(static_cast<uint8_t>(p[i] & 0x0F));
    }
    return hex;
}

int SpeakProgress::CompletedCount() const {
    int n = 0;
    const uint8_t* p = Blob();
    for (int i = 0; i < kBlobSize; ++i) {
        const uint8_t st = StatusOf(p[i]);
        if (st == kCompleted || st == kNeedsReview) {
            ++n;
        }
    }
    return n;
}

void SpeakProgress::RollStatsDay() {
    const time_t now = time(nullptr);
    if (now < 1700000000) {
        return;
    }
    const uint16_t day = static_cast<uint16_t>(now / 86400);
    const uint16_t week = static_cast<uint16_t>(day / 7);
    bool dirty = false;
    if (week != week_id_) {
        memset(week_sec_, 0, sizeof(week_sec_));
        week_id_ = week;
        dirty = true;
    }
    if (day != stats_day_) {
        today_turns_ = 0;
        today_words_ = 0;
        today_phrases_ = 0;
        today_seconds_ = 0;
        stats_day_ = day;
        dirty = true;
    }
    if (dirty) {
        Save();
    }
}

void SpeakProgress::AddSpeakTurn(bool passed, bool phrase) {
    Load();
    RollStatsDay();
    life_turns_++;
    if (today_turns_ < 0xFFFF) {
        today_turns_++;
    }
    constexpr uint16_t kSec = 15;
    life_seconds_ += kSec;
    if (today_seconds_ < 0xFFFF - kSec) {
        today_seconds_ = static_cast<uint16_t>(today_seconds_ + kSec);
    }
    const time_t now = time(nullptr);
    int wd = 0;
    if (now >= 1700000000) {
        wd = static_cast<int>(((now / 86400) + 3) % 7);
    }
    if (wd >= 0 && wd < 7 && week_sec_[wd] < 0xFFFF - kSec) {
        week_sec_[wd] = static_cast<uint16_t>(week_sec_[wd] + kSec);
    }
    if (passed) {
        if (phrase) {
            life_phrases_++;
            if (today_phrases_ < 0xFFFF) {
                today_phrases_++;
            }
        } else {
            life_words_++;
            if (today_words_ < 0xFFFF) {
                today_words_++;
            }
        }
    }
    Save();
}

uint32_t SpeakProgress::LifeTurns() const { return life_turns_; }
uint32_t SpeakProgress::LifeWords() const { return life_words_; }
uint32_t SpeakProgress::LifePhrases() const { return life_phrases_; }
uint32_t SpeakProgress::LifeSeconds() const { return life_seconds_; }
uint16_t SpeakProgress::TodayTurns() const { return today_turns_; }
uint16_t SpeakProgress::TodayWords() const { return today_words_; }
uint16_t SpeakProgress::TodayPhrases() const { return today_phrases_; }
uint16_t SpeakProgress::TodaySeconds() const { return today_seconds_; }
uint16_t SpeakProgress::WeekSeconds(int monday0) const {
    if (monday0 < 0 || monday0 > 6) {
        return 0;
    }
    return week_sec_[monday0];
}

int SpeakProgress::MasteredCount() const {
    int n = 0;
    const uint8_t* p = Blob();
    for (int i = 0; i < kBlobSize; ++i) {
        if (MasteredOf(p[i])) {
            ++n;
        }
    }
    return n;
}

std::string SpeakProgress::SummaryJson() const {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "student_slot", slot_);
    cJSON_AddStringToObject(out, "assessment", AssessName(assess_));
    cJSON_AddNumberToObject(out, "completed", CompletedCount());
    cJSON_AddNumberToObject(out, "mastered", MasteredCount());
    cJSON_AddStringToObject(out, "progress_hex", HexBlob().c_str());
    cJSON* students = cJSON_AddArrayToObject(out, "students");
    for (int s = 0; s < kMaxStudents; ++s) {
        cJSON* row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "slot", s);
        cJSON_AddStringToObject(row, "name", names_[s].c_str());
        cJSON_AddItemToArray(students, row);
    }
    char* printed = cJSON_PrintUnformatted(out);
    std::string result = printed ? printed : "{}";
    cJSON_free(printed);
    cJSON_Delete(out);
    return result;
}

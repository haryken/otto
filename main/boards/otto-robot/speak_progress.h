#ifndef SPEAK_PROGRESS_H
#define SPEAK_PROGRESS_H

#include <cstdint>
#include <string>
#include <vector>

/** Compact per-student activity progress in NVS. Not the curriculum. */
class SpeakProgress {
public:
    static constexpr int kMaxUnits = 43;
    static constexpr int kMaxActs = 8;
    static constexpr int kMaxStudents = 4;
    static constexpr int kBlobSize = kMaxUnits * kMaxActs;

    enum Status : uint8_t {
        kNotStarted = 0,
        kInProgress = 1,
        kCompleted = 2,
        kNeedsReview = 3
    };
    enum Assess : uint8_t {
        kEncourage = 0,
        kGuided = 1,
        kChallenge = 2
    };

    static SpeakProgress& Get();

    void Load();
    void Save();

    int Slot() const { return slot_; }
    void SetSlot(int slot);
    std::string StudentName(int slot) const;
    void SetStudentName(int slot, const std::string& name);

    uint8_t Assessment() const { return assess_; }
    void SetAssessment(uint8_t mode);

    static int UnitIndex(const std::string& unit);
    static int KnownActIndex(const std::string& activity_id);
    static const char* KnownActId(int act);
    static uint8_t Pack(uint8_t status, bool mastered, uint8_t item);
    static uint8_t StatusOf(uint8_t b);
    static bool MasteredOf(uint8_t b);
    static uint8_t ItemOf(uint8_t b);
    static std::string StatusName(uint8_t status);
    static const char* AssessName(uint8_t mode);
    static uint8_t ParseAssess(const std::string& name);
    enum MatchKind : uint8_t {
        kMatchNone = 0,
        kMatchNear = 1,
        kMatchExact = 2
    };

    static std::string NormalizeText(const std::string& in);
    static std::string FoldVietnamese(const std::string& in);
    static int EditDistance(const std::string& a, const std::string& b);
    static MatchKind ClassifyMatch(const std::string& heard, const std::string& expected,
                                   const std::string& meaning_vi = "");
    static bool AnswersMatch(const std::string& heard, const std::string& expected);

    uint8_t ByteAt(int unit_i, int act_i) const;
    void SetByte(int unit_i, int act_i, uint8_t value, bool save = true);
    uint8_t ByteFor(const std::string& unit, int act_i) const;

    void MarkOpened(const std::string& unit, int act_i, uint8_t item);
    void MarkItem(const std::string& unit, int act_i, uint8_t item, bool correct,
                  bool finished, bool needs_review, bool mastered);

    bool AllCompleted(const std::string& unit, const std::vector<int>& act_indices) const;

    std::string HexBlob() const;
    std::string SummaryJson() const;
    int CompletedCount() const;
    int MasteredCount() const;

    /** Count a real student utterance. passed = exact/near/pass_on (counts as word/phrase). */
    void AddSpeakTurn(bool passed, bool phrase);
    uint32_t LifeTurns() const;
    uint32_t LifeWords() const;
    uint32_t LifePhrases() const;
    uint32_t LifeSeconds() const;
    uint16_t TodayTurns() const;
    uint16_t TodayWords() const;
    uint16_t TodayPhrases() const;
    uint16_t TodaySeconds() const;
    uint16_t WeekSeconds(int monday0) const;

private:
    SpeakProgress() = default;
    uint8_t* Blob();
    const uint8_t* Blob() const;

    void RollStatsDay();

    bool loaded_ = false;
    uint8_t slot_ = 0;
    uint8_t assess_ = kGuided;
    uint8_t blob_[kMaxStudents][kBlobSize] = {};
    std::string names_[kMaxStudents];
    uint32_t life_turns_ = 0;
    uint32_t life_words_ = 0;
    uint32_t life_phrases_ = 0;
    uint32_t life_seconds_ = 0;
    uint16_t stats_day_ = 0;
    uint16_t today_turns_ = 0;
    uint16_t today_words_ = 0;
    uint16_t today_phrases_ = 0;
    uint16_t today_seconds_ = 0;
    uint16_t week_id_ = 0;
    uint16_t week_sec_[7] = {};
};

#endif

#ifndef SPEAK_LESSON_H
#define SPEAK_LESSON_H

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <vector>

/** Pika-style speaking lesson. Bookmark in NVS; JSON/images from LAN :8765 and/or HTTPS Pages. */
class SpeakLesson {
public:
    static SpeakLesson& Get();

    bool IsOn() const;
    /** Lesson running and not on the finished-ask screen. */
    bool IsTeaching() const;
    /** Teaching on Xiaozhi MQTT/WS — separate from Gemini Direct AI. */
    bool UsesXiaozhiTeach() const;
    /** Slim MCP status for Xiaozhi teach (no prompt dump, one full reply per phase). */
    std::string XiaozhiTeachStatus();
    /** Non-empty: Xiaozhi must not call heard/next/card again this turn. */
    std::string XiaozhiSkipToolJson() const;
    /** listen/detect text Xiaozhi accepts (wake-word short). Gemini keeps long cues. */
    const char* StartDetectCue() const;
    const char* SilenceDetectCue() const;
    /** Praise to speak in the same TTS breath as the next card. */
    const std::string& ComboPraise() const;
    /** Praise + new-card teacher, one TTS script for Gemini. */
    const std::string& ComboSpeak() const;
    /** English target of the card on screen (after Next). */
    std::string CurrentTeachEn() const;
    std::string CurrentTeachScript();
    /** True: do not open mic / tút until the new card English was spoken. */
    bool HoldMicForIntro() const;
    std::string BaseUrl() const;
    std::string Level() const;
    std::string UnitId() const;
    int Index() const;

    void SetBaseUrl(const std::string& url);
    /** lan + cloud stay saved; source = auto|lan|cloud (order only, the other is fallback). */
    void SetCourseSources(const std::string& lan, const std::string& cloud, const std::string& source);
    std::string Start(const std::string& level, const std::string& unit, int index = -1,
                      const std::string& backend = "", bool force_reconnect = false,
                      const std::string& activity = "");
    std::string Stop();
    std::string ResetUnit();
    std::string Next(bool force_skip = false, bool show_now = true);
    std::string CurrentCardJson();
    std::string StatusJson();
    std::string UnitJson();
    std::string ShowCurrent();
    std::string XiaozhiPrompt() const;
    std::string XiaozhiPromptEn() const;
    std::string XiaozhiDailyPrompt() const;
    std::string DirectAiExtraPrompt() const;
    /** Leave teach overlay so World Edu / Daily Device-Id is used. No reconnect. */
    void Disarm();
    /** Hi Jason / wake: force daily chat unless Start() just armed a lesson. */
    void EnsureDailyOnWake();
    /** Call when a new voice session opens. Show first card immediately. */
    void OnNewTalkSession();
    void OnAssistantTurnDone();
    void OnSpeakBegin();
    void OnTtsText(const std::string& text);
    /** Conversation ended: hide the teaching JPEG and restore the default eyes. */
    void OnTalkIdle();
    void SetProfile(int age, const std::string& likes, const std::string& dob = "");
    std::string ProfileJson() const;

    /** Persistent English style from :8080: old = World Edu, new = speaking + own daily/custom. */
    bool UsesNewIdentity() const;
    /** New style and the teaching (card) slot. */
    bool UsesTeachIdentity() const;
    /** New style and daily-chat slot (default when entering kiểu mới). */
    bool UsesDailyIdentity() const;
    const char* ActiveAiBackendName() const;
    std::string ActiveAiKey() const;
    std::string ActiveAiModel() const;
    std::string ActiveAiVoice() const;
    std::string ActiveAiInstructions() const;
    const char* EnglishStyleName() const;
    const char* EnglishSlotName() const;
    /** Device-Id for the selected new-style slot; empty when old style. */
    std::string ActiveDeviceMac() const;
    std::string ActiveClientId() const;
    void EnsureActiveClientId();
    /** New-style Xiaozhi daily: random MAC from the 20-MAC pool + new Client-Id (same as old daily). */
    void ApplyDailyPoolOnBoot();
    std::string SetEnglishStyle(bool is_new, int slot = -1);
    std::string SetNewDaily();
    std::string GoDaily(const std::string& backend = "");
    std::string SetDailyIdentity(const std::string& mac, const std::string& client_id,
                                 const std::string& ai_backend, const std::string& key, bool clear_key,
                                 const std::string& model, const std::string& voice,
                                 const std::string& instructions);
    std::string SetNewCustom(const std::string& mac, const std::string& client_id);

    /** Xiaozhi Device-Id for teach mode only (never World Edu / daily pool). */
    std::string DeviceMac() const;
    /** Xiaozhi Client-Id for teach mode only (never board UUID). */
    std::string ClientId() const;
    /** "xiaozhi" | "gemini" | "openai" | "grok" */
    std::string AiBackendName() const;
    std::string AiKey() const;
    std::string AiModel() const;
    std::string AiVoice() const;
    std::string AiInstructions() const;
    void EnsureClientId();
    std::string SetIdentity(const std::string& mac, const std::string& client_id,
                            const std::string& ai_backend, const std::string& key, bool clear_key,
                            const std::string& model, const std::string& voice,
                            const std::string& instructions);
    std::string SetTeachStt(bool english_only, const std::string& codes);
    bool SttEnglishOnly() const;
    std::string SttCodes() const;
    /** Gemini languageCodes when a lesson is on. Empty = use global nghe. */
    std::vector<std::string> TeachSttLanguageCodes() const;
    std::vector<std::string> TeachSttVocabulary() const;
    bool ShowRobotSubtitle() const;
    bool ShowSttSubtitle() const;
    std::string SetSubtitles(bool robot, bool stt);
    /** 0=Ít/minimal, 1=cơ bản, 2=chi tiết. */
    std::string SetExplainLevel(int level);
    std::string SetAssessment(const std::string& mode);
    std::string SetStudent(int slot, const std::string& name);
    std::string SetItemPhase(const std::string& phase, const std::string& card_id = "",
                             const std::string& event_id = "");
    std::string FlipSelect(const std::string& card_id, const std::string& event_id = "",
                           const std::string& session_id = "", const std::string& round_id = "");
    std::string Evaluate(const std::string& text, const std::string& verdict,
                         const std::string& source);
    void ContinueAfterFeedback();
    void OnSttText(const std::string& text);
    /** After Evaluate: one TTS (praise, or praise+next card). Safe to call twice. */
    void KickAfterGrade();
    std::string ProgressJson() const;
    std::string ActivityId() const;

private:
    SpeakLesson() = default;
    bool EnsureUnitLoaded();
    bool EnsureActivitiesLoaded();
    void BuildPlaylist();
    int LessonTotal();
    std::string FetchText(const std::string& url);
    std::string FetchFromSources(const std::string& path);
    bool ShowImage(const std::string& url);
    bool ShowImageOnce(const std::string& url);
    std::string CardJsonAt(int index, bool with_teacher);
    std::string JoinUrl(const std::string& path) const;
    std::string JoinOn(const std::string& host, const std::string& path) const;
    std::string ActiveBase() const;
    void FillSourceOrder(std::string* first, std::string* second) const;
    std::string SwapCourseHost(const std::string& url) const;
    void PinLiveFromUrl(const std::string& url);
    std::string ProfileLine() const;
    std::string ProfileLineEn() const;
    std::string IdentityReadyError() const;
    void InheritDailyAiIfNeeded();
    std::string IntroJson() const;
    std::string DoneJson() const;
    void SwitchIdentity();
    bool InLiveVoiceSession() const;
    void RerollDailyXiaozhiIdentity();
    void ParkLessonToDaily();
    void LoadNvs();
    void SaveNvs();
    void AppendActivityFields(cJSON* out, bool with_secrets);
    void LoadSentenceItem(int index);
    int CurrentActIndex() const;
    bool ReviewUnlocked();
    std::string TeacherForType(const char* type, const char* en, const char* vi,
                               const std::string& base_teacher, bool reveal);
    std::string BuildBriefing() const;
    bool WaitsForSpeech() const;
    std::string SanitizePersona(std::string text) const;
    std::string BuildFeedback(const char* kind, const std::string& heard) const;
    std::string BuildComboScript();
    void FlushPendingShow();
    void AppendTeachSpeech(cJSON* out, cJSON* card, const char* type, const char* en,
                           const char* vi, const char* teacher, bool hide_answer, bool reveal,
                           bool with_teacher, std::string* teacher_out);
    bool IsFlip() const;
    void ResetFlipSession();
    void FlipInitFromActivity();
    void FlipSetupRound();
    void FlipAdvancePreview();
    void FlipResolveCurrent(bool correct, bool first_try, bool needs_review);
    bool FlipCardResolved(const std::string& id) const;
    bool FlipCardInRound(const std::string& id) const;
    std::string FlipFocusCardId() const;
    std::string FlipAnswerForCard(const std::string& card_id) const;
    void FlipAppendSnapshot(cJSON* out) const;
    void FlipSaveNvs();
    bool FlipRestoreIfPossible();
    int FlipDeckIndex(const std::string& id) const;
    static void FlipShuffle(std::vector<std::string>* ids, uint32_t seed);

    bool loaded_nvs_ = false;
    bool on_ = false;
    bool style_new_ = false;
    uint8_t new_slot_ = 1;  // 0=teach, 1=daily (default), 2=custom
    std::string base_ = "http://192.168.100.161:8765";
    std::string cloud_ = "https://emun.pages.dev";
    uint8_t src_ = 0;  // 0=auto (LAN then Pages), 1=LAN first, 2=Pages first
    std::string live_;  // last host that returned JSON/JPEG (RAM)
    std::string level_ = "a1";
    std::string unit_ = "u01";
    int index_ = 0;
    int age_ = 0;
    std::string dob_;
    std::string likes_;
    std::string mac_;
    std::string uuid_;
    std::string dmac_;
    std::string duid_;
    std::string cmac_;
    std::string cuid_;
    std::string dai_ = "xiaozhi";
    std::string akey_;
    std::string amod_;
    std::string avoi_;
    std::string ains_;
    bool stt_english_ = true;
    std::string stt_codes_ = "en-US";
    std::string ddai_ = "xiaozhi";
    std::string dkey_;
    std::string dmod_;
    std::string dvoi_;
    std::string dins_;
    std::string cached_json_;
    std::string cached_key_;
    bool intro_pending_ = false;
    bool intro_hold_ = false;
    bool intro_ready_ = false;
    bool identity_pending_ = false;
    bool teach_armed_ = false;  // RAM only: Start() this session, not leftover NVS
    bool done_ask_ = false;     // RAM: unit finished, asking restart this topic
    bool sub_robot_ = false;
    bool sub_stt_ = false;
    uint8_t explain_level_ = 0;  // 0=minimal/Ít, 1=basic, 2=detailed
    std::string activity_id_;
    std::string activity_type_;
    std::string image_field_;
    std::string cached_act_json_;
    std::string cached_act_key_;
    std::vector<std::string> playlist_;
    uint8_t attempts_ = 0;
    uint8_t item_phase_ = 2;  // 1=preview 2=show 3=listen 4=hint 5=reveal
    bool scoring_graded_ = false;
    bool allow_personal_ = false;
    bool activity_locked_ = false;
    bool last_reveal_ = false;
    bool last_correct_ = false;
    bool needs_review_act_ = false;
    bool item_ok_ = false;
    int items_correct_ = 0;
    int items_wrong_ = 0;
    std::string last_hint_;
    std::string last_verdict_;
    std::string last_feedback_;
    std::string last_heard_;
    uint8_t last_match_ = 0;
    uint8_t praise_i_ = 0;
    bool wait_for_speech_ = false;
    bool feedback_pending_ = false;
    bool feedback_spoken_ = false;
    bool advance_after_feedback_ = false;
    bool just_auto_advanced_ = false;
    bool skip_turn_done_ = false;
    int64_t skip_turn_done_us_ = 0;
    uint8_t xz_status_hits_ = 0;
    int64_t last_xz_status_us_ = 0;
    std::string combo_praise_;
    std::string combo_script_;
    bool pending_show_ = false;
    bool card_intro_heard_ = false;
    uint8_t intro_recue_ = 0;
    bool xz_got_card_ = false;
    bool xz_kick_teacher_ = false;
    bool xz_hold_tools_ = false;
    int64_t speak_begin_us_ = 0;
    bool xz_wrong_word_ = false;
    std::string pattern_en_;
    std::string answer_en_;
    std::string ask_q_;
    std::string hidden_id_;
    std::vector<std::string> group_ids_;
    std::string briefing_;
    bool briefing_pending_ = false;
    uint8_t flip_phase_ = 0;  // 0 off, 1 preview, 2 await_select, 3 await_answer, 4 done
    uint8_t flip_round_n_ = 0;
    uint8_t flip_preview_i_ = 0;
    bool flip_review_round_ = false;
    bool flip_review_used_ = false;
    bool flip_any_review_ = false;
    bool flip_all_first_ok_ = true;
    uint32_t flip_seed_ = 0;
    std::vector<std::string> flip_deck_;
    std::vector<std::string> flip_round_;
    std::vector<std::string> flip_resolved_;
    std::vector<std::string> flip_review_;
    std::string active_card_id_;
    std::string flip_session_id_;
    std::string flip_round_id_;
    std::string last_flip_event_id_;
};

std::string OttoSpeakDirectAiPrompt();
std::vector<std::string> OttoSpeakTeachSttCodes();
std::vector<std::string> OttoSpeakTeachSttVocab();

#endif

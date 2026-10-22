#ifndef SPEAK_LESSON_H
#define SPEAK_LESSON_H

#include <cstdint>
#include <string>

/** Pika-style speaking lesson. Bookmark in NVS; JSON/images from LAN HTTP (PC :8765). */
class SpeakLesson {
public:
    static SpeakLesson& Get();

    bool IsOn() const;
    std::string BaseUrl() const;
    std::string Level() const;
    std::string UnitId() const;
    int Index() const;

    void SetBaseUrl(const std::string& url);
    std::string Start(const std::string& level, const std::string& unit, int index = -1,
                      const std::string& backend = "", bool force_reconnect = false);
    std::string Stop();
    std::string ResetUnit();
    std::string Next();
    std::string CurrentCardJson();
    std::string StatusJson();
    std::string UnitJson();
    std::string ShowCurrent();
    std::string XiaozhiPrompt() const;
    std::string XiaozhiPromptEn() const;
    std::string DirectAiExtraPrompt() const;
    /** Leave teach overlay so World Edu / Daily Device-Id is used. No reconnect. */
    void Disarm();
    /** Hi Jason / wake: force daily chat unless Start() just armed a lesson. */
    void EnsureDailyOnWake();
    /** Call when a new voice session opens (Hi Jason). Intro plays before the first card. */
    void OnNewTalkSession();
    /** After the intro TTS finishes, the next card/show may start the unit. */
    void OnAssistantTurnDone();
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
    bool ShowRobotSubtitle() const;
    bool ShowSttSubtitle() const;
    std::string SetSubtitles(bool robot, bool stt);
    /** 0=chỉ nói, 1=nghĩa cơ bản, 2=nghĩa chi tiết. */
    std::string SetExplainLevel(int level);

private:
    SpeakLesson() = default;
    bool EnsureUnitLoaded();
    std::string FetchText(const std::string& url);
    bool ShowImage(const std::string& url);
    std::string CardJsonAt(int index, bool with_teacher);
    std::string JoinUrl(const std::string& path) const;
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

    bool loaded_nvs_ = false;
    bool on_ = false;
    bool style_new_ = false;
    uint8_t new_slot_ = 1;  // 0=teach, 1=daily (default), 2=custom
    std::string base_ = "http://192.168.100.161:8765";
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
    uint8_t explain_level_ = 0;  // 0=off, 1=basic, 2=detailed
};

std::string OttoSpeakDirectAiPrompt();

#endif

#ifndef DIRECT_AI_PROTOCOL_H
#define DIRECT_AI_PROTOCOL_H

#include "protocol.h"
#include "direct_ai_config.h"

#include <web_socket.h>
#include <esp_ae_rate_cvt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/*
 * Talks to OpenAI Realtime, Grok Voice or Gemini Live directly over a TLS
 * WebSocket (no Xiaozhi server). Server events are translated into the same
 * tts / stt JSON the Xiaozhi server sends, so Application needs no changes to
 * its state machine.
 *
 * Mic: raw 16 kHz PCM from AudioService (Opus is skipped), resampled to
 *      24 kHz for OpenAI / Grok, base64 in JSON.
 * Speaker: 24 kHz PCM from the model, resampled to the codec rate and queued
 *      on AudioService after Application entered the Speaking state.
 * Tools: McpServer tools are offered as functions; calls run through
 *      McpServer and the replies come back through SendMcpMessage().
 */
class DirectAiProtocol : public Protocol {
public:
    DirectAiProtocol();
    ~DirectAiProtocol();

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;

private:
    enum InboxKind { kInboxServer, kInboxMcpReply, kInboxAbort, kInboxClosed };
    struct InboxItem {
        InboxKind kind = kInboxServer;
        uint32_t conn = 0;
        std::string data;
    };
    struct PendingCall {
        std::string call_id;    // OpenAI call_id / Gemini function call id
        std::string wire_name;  // tool name as the model sent it
    };
    struct ToolResult {
        std::string call_id;
        std::string wire_name;
        std::string output;
    };
    enum GateState { kGateWaiting, kGateOpen, kGateFailed };

    // Lambdas scheduled on the main task hold a weak_ptr to this token.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    // ---- main task (except where noted) ----
    DirectAiConfig config_;
    std::atomic<DirectAiBackend> backend_{DirectAiBackend::kOff};
    std::atomic<uint32_t> config_generation_{0};
    bool no_search_[4] = {false, false, false, false};
    std::unique_ptr<WebSocket> ws_;  // replaced on the main task only
    std::mutex ws_mutex_;            // guards ws_ for senders on other tasks
    std::atomic<uint32_t> conn_id_{0};
    std::atomic<bool> ready_{false};
    EventGroupHandle_t events_ = nullptr;
    std::mutex setup_mutex_;
    std::string setup_error_;
    std::string resume_handle_;      // Gemini session resumption (worker writes)
    esp_ae_rate_cvt_handle_t up_resampler_ = nullptr;
    std::atomic<int> appended_ms_{0};
    std::atomic<int> silence_prompts_{0};
    std::atomic<int64_t> last_activity_us_{0};

    // ---- shared with the worker ----
    std::atomic<bool> speaking_{false};
    std::atomic<bool> muted_{false};
    std::atomic<int> gate_{kGateWaiting};
    std::atomic<uint32_t> speak_seq_{0};

    // ---- inbox / worker ----
    std::mutex inbox_mutex_;
    std::condition_variable inbox_cv_;
    std::deque<InboxItem> inbox_;
    size_t inbox_bytes_ = 0;
    bool stop_ = false;
    TaskHandle_t worker_ = nullptr;
    SemaphoreHandle_t worker_done_ = nullptr;

    // ---- worker only ----
    int codec_rate_ = 16000;
    esp_ae_rate_cvt_handle_t down_resampler_ = nullptr;
    int down_rate_ = 0;
    std::map<int, PendingCall> pending_;
    std::vector<ToolResult> gemini_results_;
    int next_rpc_id_ = 1000000;
    bool response_active_ = false;
    bool need_followup_ = false;
    bool abort_turn_ = false;
    bool end_requested_ = false;
    bool gemini_in_turn_ = false;
    bool gemini_turn_complete_ = false;
    bool gemini_tool_followup_ = false;
    int64_t gemini_grace_deadline_ = 0;
    std::string user_text_;
    std::string reply_text_;

    // Connection (main task)
    bool Connect(bool search, std::string* error);
    void CloseSocket();
    void OnSocketClosed(uint32_t conn);
    void NotifyClosed();
    bool Send(const std::string& text);
    bool SendText(const std::string& text) override;
    void SendJson(cJSON* root);
    void SendUserText(const std::string& text);
    void RequestClose();

    // Session setup (main task)
    std::string BuildInstructions() const;
    std::string TranscribeLanguage() const;
    cJSON* BuildTools(bool gemini) const;
    std::string BuildOpenAISetup(bool beta) const;
    std::string BuildGrokSetup(bool search) const;
    std::string BuildGeminiSetup(bool search);

    // Worker
    void Push(InboxItem&& item);
    void WorkerLoop();
    void Tick();
    void HandleServer(const InboxItem& item);
    void HandleRealtime(const cJSON* root);
    void HandleGemini(const cJSON* root);
    void HandleMcpReply(const std::string& payload);
    void HandleAbort();
    void ResetTurn();
    void RunTool(const std::string& call_id, const std::string& wire_name, cJSON* args);
    void SendRealtimeToolOutput(const std::string& call_id, const std::string& output);
    void SendGeminiToolResponse();
    void TryFollowUp();
    void MaybeFinishRealtimeTurn();
    void FinishTurn();
    void PlayAudio(const char* b64, size_t len, int rate);
    void BeginSpeaking();
    bool WaitGate();
    void OnUserSpeech();
    void ShowError(const std::string& message);

    // Events for Application (same shape as the Xiaozhi server's)
    void Emit(cJSON* root);
    void EmitTts(const char* state);
    void EmitStt(const std::string& text);
    void EmitAssistant(const std::string& text);
    void FlushStt();
    void FlushTranscripts();
};

#endif // DIRECT_AI_PROTOCOL_H

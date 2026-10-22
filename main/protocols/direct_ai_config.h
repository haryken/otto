#ifndef DIRECT_AI_CONFIG_H
#define DIRECT_AI_CONFIG_H

#include <cJSON.h>
#include <cstdint>
#include <string>

// Direct AI mode: the device talks to OpenAI Realtime / Grok Voice / Gemini Live
// itself, without the Xiaozhi server. Settings live in NVS namespace "direct_ai"
// and are edited from the Self-Control page (:8080, tab AI).
enum class DirectAiBackend {
    kOff,     // normal Xiaozhi server
    kOpenAI,
    kGrok,
    kGemini,
};

struct DirectAiConfig {
    DirectAiBackend backend = DirectAiBackend::kOff;
    std::string key;
    std::string model;
    std::string voice;
    std::string language;      // "vi", "en", ... or "auto"
    std::string instructions;  // owner's extra instructions
    bool search = true;        // web search tool (Grok / Gemini)

    /** Settings of the saved backend (key/model/voice of that backend). */
    static DirectAiConfig Load();
    static DirectAiBackend LoadBackend();

    static const char* BackendName(DirectAiBackend backend);
    static DirectAiBackend ParseBackend(const std::string& name);

    /** Backend in use. Xiaozhi <-> AI needs a reboot; AI <-> AI applies per session. */
    static void SetRunningBackend(DirectAiBackend backend);
    static DirectAiBackend RunningBackend();

    /** Bumped on every save so an open session can pick up new settings. */
    static uint32_t Generation();

    /** JSON for GET /api/ai. API keys are never returned, only masked hints. */
    static std::string ToWebJson();
    /**
     * Apply POST /api/ai. Returns false with *error on bad input.
     * *backend_changed tells the caller a reboot is needed.
     */
    static bool ApplyWebJson(const cJSON* root, bool* backend_changed, std::string* error);
};

#endif // DIRECT_AI_CONFIG_H

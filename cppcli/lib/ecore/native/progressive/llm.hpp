#ifndef PROGRESSIVE_LLM_HPP
#define PROGRESSIVE_LLM_HPP

#include <string>
#include <vector>

namespace progressive {

enum class LlmProvider { OpenAI, Anthropic };

struct LlmConfig {
    LlmProvider provider = LlmProvider::OpenAI;
    std::string apiEndpoint;     // e.g. "https://api.openai.com/v1/chat/completions"
    std::string apiToken;
    std::string model;           // "gpt-4o-mini" or "claude-3-haiku-20240307"
    std::string systemPrompt;    // optional system prompt
    float temperature = 0.7f;
    int maxTokens = 1024;
};

struct LlmRequest {
    std::string prompt;
    LlmConfig config;
    bool broadcastToRoom = false; // /llmp: visible to all, /llm: visible to user only
};

struct LlmResponse {
    bool success = false;
    std::string text;
    std::string errorMessage;
    int statusCode = 0;
    int tokensUsed = 0;
};

// Build request body for OpenAI-compatible API.
std::string buildOpenAiRequestBody(const LlmConfig& config, const std::string& prompt);

// Build request body for Anthropic API.
std::string buildAnthropicRequestBody(const LlmConfig& config, const std::string& prompt);

// Build the appropriate request body based on provider.
std::string buildLlmRequestBody(const LlmConfig& config, const std::string& prompt);

// Build required headers for the API call.
std::string buildLlmHeaders(const LlmConfig& config);

// Parse OpenAI response.
LlmResponse parseOpenAiResponse(const std::string& body, int statusCode);

// Parse Anthropic response.
LlmResponse parseAnthropicResponse(const std::string& body, int statusCode);

// Parse response based on provider.
LlmResponse parseLlmResponse(const std::string& body, int statusCode, LlmProvider provider);

// Format the broadcast message for /llmp: "On prompt: XY, LLM says: YZ"
std::string formatLlmBroadcastMessage(const std::string& prompt, const std::string& response);

// ---- Duplicate Name Detection ----

struct DuplicateNameInfo {
    std::string mxid;
    std::string displayName;
    bool hasDuplicates = false;  // other users share this display name
    bool showMxid = true;        // user-level override: false = hide MXID
};

// Detect duplicate display names in a list of users.
// For each user where displayName appears more than once, mark hasDuplicates=true.
std::vector<DuplicateNameInfo> detectDuplicateNames(
    const std::vector<DuplicateNameInfo>& users
);

// Format a display name with optional MXID suffix: "Alice (@alice:server)"
std::string formatUserDisplayName(const std::string& displayName, const std::string& mxid, bool showMxid);

// ---- Per-User MXID Visibility ----

class UserMxidVisibility {
public:
    // Hide MXID for a specific user.
    void hideMxid(const std::string& mxid);

    // Show MXID for a specific user (default behavior).
    void showMxid(const std::string& mxid);

    // Check if MXID should be shown for this user.
    bool isVisible(const std::string& mxid) const;

    // Remove user from overrides.
    void reset(const std::string& mxid);

    void clear();
    size_t count() const { return hidden_.size(); }

    std::string exportJson() const;
    void importJson(const std::string& json);

private:
    // Store only hidden users (all shown by default)
    std::vector<std::string> hidden_;
};

inline bool hasToolCalls(const std::string& /*response*/) { return false; }
inline std::string extractTextAnswer(const std::string& /*response*/) { return ""; }
inline std::string getAgentToolsSchema() { return "[]"; }

} // namespace progressive

#endif // PROGRESSIVE_LLM_HPP

#pragma once

// Android-parity /agent: an LLM agent loop with Matrix tools
// (read_messages, send_message, edit_message, search_messages, list_users,
// get_user_info, react_to_message, get_room_info, send_direct_message).
// The same engine backs the CLI `matrixcli agent` command and the TUI
// `/agent` slash command.
//
// The backend is fully configurable — the order of precedence:
//   1. the values already set by the caller (the CLI flags)
//   2. ~/.config/matrixcli/agent.json (provider/endpoint/model/key/proxy)
//   3. the provider presets (openai, anthropic, ollama, deepseek, mimo, ...)
//   4. OPENAI_API_KEY / ANTHROPIC_API_KEY environment variables
//   5. per-provider defaults (endpoint + model)

#include <functional>
#include <memory>
#include <string>

namespace matrixcli { namespace matrix { class Client; } }

namespace matrixcli { namespace matrixagent {

struct Config {
    std::string provider = "";  // "openai" | "anthropic" | a preset name
    std::string endpoint;       // base URL; "" = per-provider default
    std::string model;          // "" = per-provider default
    std::string key;
    std::string proxy;          // SOCKS5 "host:port" (Tor), "" = direct
    int maxIterations = 10;
    int maxTokens = 0;          // the completion budget; 0 = unlimited (the
                                // provider decides — the OpenAI-compatible
                                // path omits the field entirely; Anthropic
                                // requires one, so 0 becomes 8192 there)
    double temperature = 0.2;   // always sent (the OpenAI-compatible path)
    double topP = 0.0;          // 0 = the field is omitted
    std::string reasoning;      // the reasoning_effort ("" = omit):
                                // low | medium | high
    bool debugLlm = false;      // dump the raw SSE payloads to stderr
    bool verbose = false;
    std::string extraSystem;    // appended to the built-in system prompt
};

struct Result {
    bool ok = false;
    std::string answer;
    std::string error;
    int iterations = 0;
};

// The Matrix operations the agent tools need. Two implementations exist:
// the pcore/ecore session (the CLI `matrixcli agent`) and the lib/matrix
// client (the TUI — its own logged-in account).
class Backend {
public:
    virtual ~Backend() = default;
    // Returns a JSON result string on success, {"error": ...} on failure.
    virtual std::string sendMessage(const std::string& roomId,
                                    const std::string& text) = 0;
    virtual std::string editMessage(const std::string& roomId,
                                    const std::string& eventId,
                                    const std::string& newText) = 0;
    virtual std::string listUsers(const std::string& roomId) = 0;
    virtual std::string getUserInfo(const std::string& userId) = 0;
    virtual std::string react(const std::string& roomId,
                              const std::string& eventId,
                              const std::string& emoji) = 0;
    virtual std::string sendDirectMessage(const std::string& userId,
                                          const std::string& text) = 0;
    virtual std::string selfUserId() const = 0;
};

std::unique_ptr<Backend> makeEcoreBackend();
std::unique_ptr<Backend> makeMatrixBackend(matrix::Client* client);

// Fill in the defaults for the empty Config fields (agent.json, provider
// presets, env keys, per-provider endpoint/model). Never overwrites the
// values that are already set.
void applyDefaults(Config& cfg);

// One agent run with the Matrix tools. `roomId` may be empty or a room
// name (resolved against the local cache). `log` receives the progress
// lines ("[agent] iteration N ..." / "[agent] tool: ...") when verbose.
Result run(const Config& cfg, Backend& backend, const std::string& task,
           const std::string& roomId,
           const std::function<void(const std::string&)>& log = {});

// A single chat completion (the /llm path). Returns the assistant text;
// on failure returns "" and fills `error`.
std::string complete(const Config& cfg, const std::string& system,
                     const std::string& prompt, std::string& error);

// ---- rich completions (the llm CLI metadata) ----

struct ChatMessage {
    std::string role;     // "user" | "assistant"
    std::string content;
};

struct Completion {
    bool ok = false;
    std::string text;
    std::string error;
    std::string model;             // the model actually used
    std::string reasoning;         // the reasoning_content (r-models)
    int64_t promptTokens = 0;
    int64_t completionTokens = 0;
    int64_t ts = 0;                // request timestamp (ms since epoch)
};

// One completion with the usage metadata (tokens, model, timestamp).
Completion completeEx(const Config& cfg, const std::string& system,
                      const std::string& prompt, std::string& error);

// Multi-turn completion: the whole conversation is sent to the model.
Completion chat(const Config& cfg, const std::string& system,
                const std::vector<ChatMessage>& messages, std::string& error);

// The SSE-streamed completion: the tokens are fed to `onToken` as they
// arrive (the OpenAI-compatible providers; Anthropic falls back to the
// non-streaming path). The reasoning pieces go to `onReasoning`. The
// usage comes from the final chunk when the provider reports it.
Completion stream(const Config& cfg, const std::string& system,
                  const std::vector<ChatMessage>& messages,
                  const std::function<void(const std::string&)>& onToken = {},
                  const std::function<void(const std::string&)>& onReasoning = {});

}} // namespace matrixcli::matrixagent

#pragma once

// The local coding-agent engine (opencode/codex-style): an LLM loop with
// filesystem + shell tools, plan tracking, configurable trust levels,
// permission glob rules, subagents, sessions and a sandboxed shell.
// Both providers (OpenAI-compatible and Anthropic) are supported with
// native tool calls.

#include <functional>
#include <string>
#include <vector>

namespace matrixcli { namespace agenttools {

struct PermissionRule {
    std::string tool;    // read | write | edit | shell | apply_patch | "*"
    std::string glob;    // fnmatch pattern over the path (or the command)
    std::string action;  // allow | ask | deny
};

struct McpServer {
    std::string name;
    std::string command;  // e.g. "npx -y @modelcontextprotocol/server-files ."
};

struct Config {
    std::string provider = "openai";        // "openai" | "anthropic"
    std::string endpoint;                   // base URL, e.g. https://api.openai.com
    std::string model;                      // gpt-4o-mini / claude-3-5-haiku-...
    std::string key;
    std::string trust = "ask";              // allow | ask | deny
    std::vector<std::string> allowPrefixes; // shell prefixes that always run
    std::vector<std::string> denyPrefixes;  // shell prefixes that never run
    std::vector<PermissionRule> rules;      // per-tool glob rules (last wins)
    std::string cwd;                        // working directory for the tools
    int maxIterations = 10;
    int maxDepth = 3;                       // subagent nesting limit
    std::string subagentType;               // "" | "explore" | "general" (subagents)
    std::string proxy;                      // SOCKS5 proxy "host:port" (Tor) or ""
    std::string sandbox = "off";            // "" | "bwrap"
    std::vector<McpServer> mcpServers;
    // Plan mode: the agent only reads, plans and writes a plan file —
    // no writes to other files, no shell (the plan-approval flow).
    bool planMode = false;
    std::string planFile;                   // where the plan is written
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string args;   // JSON object string
};

struct Message {
    std::string role;               // system | user | assistant | tool
    std::string content;            // text (empty for tool-call-only rows)
    std::vector<ToolCall> calls;    // assistant tool calls
    std::string toolCallId;         // for role == "tool"
    std::string toolName;           // for role == "tool"
};

// The user's answer to a permission prompt.
enum class ConfirmVerdict { Decline = 0, Once, Session, Always };

struct CronJob {
    std::string id;
    std::string spec;       // "30m" | "every 2h" | the cron expr | ISO time
    std::string prompt;
    int64_t nextRun = 0;    // the epoch seconds
    std::string monitorUrl; // the change-detection URL ("" = none)
    std::string monitorHash; // the last seen SHA-256 ("" = the baseline next run)
};

// The hermes-style schedule parser: the next epoch seconds, -1 on error.
int64_t nextRunFromSpec(const std::string& spec, int64_t now);

// The cron jobs store (the JSON file).
void saveCronJobs(const std::string& path, const std::vector<CronJob>& jobs);
bool loadCronJobs(const std::string& path, std::vector<CronJob>& jobs);

// The hardline dangerous-command patterns (they deny even under trust allow).
bool isDangerousCommand(const std::string& cmd);

// The e-stop sentinel.
bool eStopEngaged(const std::string& path = ".agent-estop");

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int iterations = 0;
    bool streamed = false;  // the tokens were already printed live
};

// One agentic run: the prompt is answered, tool calls are executed (the
// shell honours the trust policy — `confirm` is invoked for "ask";
// denyPrefixes win over allowPrefixes which win over the level). The
// `ask` callback handles the question tool (JSON: {questions:[...]}).
// `history` carries the conversation across calls (the interactive mode).
Result run(const Config& cfg, const std::string& prompt,
           std::vector<Message>& history,
           const std::function<int(const std::string& cmd)>& confirm,
           const std::function<std::string(const std::string& questionsJson)>& ask,
           const std::function<void(const std::string&)>& log,
           const std::function<void(const std::string&)>& onToken = {});

// A single tool execution (used by run; exposed for testing).
std::string executeTool(const Config& cfg, const std::string& name,
                        const std::string& argsJson,
                        const std::function<int(const std::string&)>& confirm,
                        const std::function<std::string(const std::string&)>& ask,
                        const std::function<void(const std::string&)>& log,
                        int depth = 0);

// Session persistence: the history to/from a JSON file.
void saveSession(const std::string& path, const std::vector<Message>& history);
bool loadSession(const std::string& path, std::vector<Message>& history);

}} // namespace matrixcli::agenttools

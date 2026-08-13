#pragma once

// The local coding-agent engine (opencode/codex-style): an LLM loop with
// filesystem + shell tools and configurable trust levels. Both providers
// (OpenAI-compatible and Anthropic) are supported with native tool calls.

#include <functional>
#include <string>
#include <vector>

namespace matrixcli { namespace agenttools {

struct Config {
    std::string provider = "openai";        // "openai" | "anthropic"
    std::string endpoint;                   // base URL, e.g. https://api.openai.com
    std::string model;                      // gpt-4o-mini / claude-3-haiku-...
    std::string key;
    std::string trust = "ask";              // allow | ask | deny
    std::vector<std::string> allowPrefixes; // shell prefixes that always run
    std::vector<std::string> denyPrefixes;  // shell prefixes that never run
    std::string cwd;                        // working directory for the tools
    int maxIterations = 10;
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

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int iterations = 0;
};

// One agentic run: the prompt is answered, tool calls are executed (the
// shell honours the trust policy — the confirm callback is invoked for
// "ask"; denyPrefixes win over allowPrefixes which win over the level).
// `history` carries the conversation across calls (the interactive mode).
Result run(const Config& cfg, const std::string& prompt,
           std::vector<Message>& history,
           const std::function<bool(const std::string& cmd)>& confirm,
           const std::function<void(const std::string&)>& log);

// A single tool execution (used by run; exposed for testing).
std::string executeTool(const Config& cfg, const std::string& name,
                        const std::string& argsJson,
                        const std::function<bool(const std::string&)>& confirm);

}} // namespace matrixcli::agenttools

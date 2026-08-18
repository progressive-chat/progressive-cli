#include "agent_tools.hpp"
#include "globals.hpp"

#include "../lib/http/http.hpp"
#include "../lib/json/json.hpp"
#include "../lib/util/llm_presets.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fnmatch.h>
#include <fstream>
#include <glob.h>
#include <map>
#include <memory>
#include <thread>
#include <iomanip>
#include <mutex>
#include <unordered_set>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <unistd.h>

namespace matrixcli { namespace agenttools {

using nlohmann::json;

struct McpConn {
    FILE* f = nullptr;
    std::string error;
    int nextId = 2;

    bool start(const McpServer& srv) {
        f = popen(srv.command.c_str(), "r+");
        if (!f) {
            error = "cannot start MCP server: " + srv.command;
            return false;
        }
        json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                     {"params",
                      {{"protocolVersion", "2024-11-05"},
                       {"capabilities", json::object()},
                       {"clientInfo", {{"name", "matrixcli"}, {"version", "0.5.0"}}}}}};
        json resp = rpc(init);
        if (resp.value("error", json()).is_object()) {
            error = "MCP initialize failed: " + resp["error"].dump();
            return false;
        }
        return true;
    }

    json rpc(const json& req) {
        std::string line = req.dump() + "\n";
        if (!f || fwrite(line.data(), 1, line.size(), f) != line.size()) {
            return json();
        }
        fflush(f);
        char buf[65536];
        std::string acc;
        int guard = 0;
        while (fgets(buf, sizeof(buf), f) && guard++ < 5000) {
            acc += buf;
            try {
                json j = json::parse(acc);
                return j;
            } catch (...) {
                continue;
            }
        }
        return json();
    }

    ~McpConn() {
        if (f) pclose(f);
    }
};

// ---- the LLM adapters ----
static std::string llmSimple(const Config& cfg, const std::string& prompt) {
    http::Client httpClient;
    httpClient.setTimeout(120);
    if (!cfg.proxy.empty()) {
        auto colon = cfg.proxy.rfind(':');
        if (colon != std::string::npos) {
            http::ProxyConfig pc;
            pc.type = http::ProxyType::SOCKS5;
            pc.host = cfg.proxy.substr(0, colon);
            try { pc.port = std::stoi(cfg.proxy.substr(colon + 1)); } catch (...) {}
            if (pc.enabled()) httpClient.setProxy(pc);
        }
    }
    json reqBody;
    http::Request req;
    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", prompt}});
    if (cfg.provider == "anthropic") {
        reqBody = {{"model", cfg.model}, {"messages", messages},
                   {"max_tokens", 1024}};
        req.url = baseEndpoint(cfg) + "/v1/messages";
        req.headers["x-api-key"] = cfg.key;
        req.headers["anthropic-version"] = "2023-06-01";
        req.headers["content-type"] = "application/json";
    } else {
        reqBody = {{"model", cfg.model}, {"messages", messages}};
        req.url = baseEndpoint(cfg) + "/v1/chat/completions";
        req.headers["authorization"] = "Bearer " + cfg.key;
        req.headers["content-type"] = "application/json";
    }
    req.method = "POST";
    req.body = reqBody.dump();
    http::Response resp = httpClient.doRequest(req);
    if (!resp.ok()) return "";
    try {
        json parsed = json::parse(resp.body);
        Message m = cfg.provider == "anthropic"
                        ? parseAnthropicResponse(parsed)
                        : parseOpenAiResponse(parsed);
        return m.content;
    } catch (...) {
        return "";
    }
}

// ---- the provider presets + the usage accounting ----
static std::string goalContinuationBlock(const GoalState& g) {
    std::string out = "[Continuing toward your standing goal]\nGoal: "
                    + g.goal + "\n\n";
    if (!g.contract.empty()) {
        out += "Completion contract:\n" + g.contract + "\n\n";
    }
    if (!g.subgoals.empty()) {
        out += "Additional criteria the user added mid-loop:\n";
        for (size_t i = 0; i < g.subgoals.size(); ++i) {
            out += std::to_string(i + 1) + ". " + g.subgoals[i] + "\n";
        }
        out += "\n";
    }
    out += "Continue working toward the goal (and every additional "
           "criterion). Take the next concrete step. Before claiming the "
           "goal is done, satisfy the Verification criterion and show the "
           "concrete evidence (command output, file contents, test result). "
           "If you are blocked and need input from the user, say so clearly "
           "and stop.";
    return out;
}

// The contract template (the hermes draft_contract shape).
std::string executeTool(const Config& cfg, const std::string& name,
                        const std::string& argsJson,
                        const std::function<int(const std::string&)>& confirm,
                        const std::function<std::string(const std::string&)>& ask,
                        const std::function<void(const std::string&)>& log,
                        int depth) {
    json args = json::object();
    if (!argsJson.empty()) {
        try {
            args = json::parse(argsJson);
        } catch (...) {
            return "error: bad arguments JSON";
        }
    }
    auto str = [&](const char* k) { return args.value(k, ""); };
    auto i64 = [&](const char* k, int dflt) {
        return args.contains(k) && args[k].is_number()
                   ? args[k].get<int>() : dflt;
    };
    if (name == "shell") {
        std::string cmd = str("command");
        if (cmd.empty()) return "error: empty command";
        if (cfg.planMode) {
            return "denied: plan mode — the plan is written to the plan "
                   "file, no shell is run yet";
        }
        // The hardline patterns deny even under trust allow / YOLO.
        if (!matrixcli::g_interrupted.load()) {
            return "interrupted (Ctrl+C)";
        }
        if (matrixcli::g_agentInterrupt.load()) {
            return "interrupted (Esc)";
        }
        if (isDangerousCommand(cmd)) {
            return "hardline block: dangerous command refused — " + cmd;
        }
        Verdict v = checkTrust(cfg, cmd);
        if (v == Verdict::Deny) return "denied by the trust policy: " + cmd;
        if (v == Verdict::Ask && confirm) {
            int verdict = confirm(cmd);
            if (verdict == static_cast<int>(ConfirmVerdict::Decline)) {
                return "declined by the user: " + cmd;
            }
        }
        return shellCmd(cmd, i64("timeout", 60), str("workdir"), cfg.sandbox);
    }
    if (name == "process") {
        if (cfg.planMode) return "denied: plan mode — no processes yet";
        if (cfg.subagentType == "explore")
            return "denied: the explore subagent is read-only";
        return processTool(cfg, argsJson, confirm);
    }
    if (name == "read_file") {
        std::string p = str("path");
        Verdict v = checkPermission(cfg, "read", p);
        if (v == Verdict::Deny) return "denied by the permission rules: " + p;
        if (v == Verdict::Ask && confirm) {
            if (confirm("read " + p) == static_cast<int>(ConfirmVerdict::Decline)) {
                return "declined by the user: " + p;
            }
        }
        return readFile(p, i64("offset", 1), i64("limit", 2000));
    }
    if (name == "write_file") {
        std::string p = str("path");
        if (cfg.planMode && p != cfg.planFile) {
            return "denied: plan mode — only the plan file may be written";
        }
        Verdict v = checkPermission(cfg, "write", p);
        if (v == Verdict::Deny) return "denied by the permission rules: " + p;
        if (v == Verdict::Ask && confirm) {
            if (confirm("write " + p) == static_cast<int>(ConfirmVerdict::Decline)) {
                return "declined by the user: " + p;
            }
        }
        return writeFile(p, str("content"));
    }
    if (name == "edit_file") {
        std::string p = str("path");
        if (cfg.planMode && p != cfg.planFile) {
            return "denied: plan mode — only the plan file may be edited";
        }
        Verdict v = checkPermission(cfg, "edit", p);
        if (v == Verdict::Deny) return "denied by the permission rules: " + p;
        if (v == Verdict::Ask && confirm) {
            if (confirm("edit " + p) == static_cast<int>(ConfirmVerdict::Decline)) {
                return "declined by the user: " + p;
            }
        }
        return editFile(p, str("old"), str("new"),
                        args.value("replaceAll", false));
    }
    if (name == "apply_patch") {
        return cfg.planMode ? "denied: plan mode — no patches yet"
                            : applyPatch(str("patch_text"));
    }
    if (name == "glob") return globFiles(str("pattern"));
    if (name == "grep") return grepFiles(str("pattern"), str("path"));
    if (name == "webfetch") return webFetch(str("url"), i64("maxChars", 30000));
    if (name == "todo") return todoTool(argsJson);
    if (name == "question") {
        if (!ask) return "error: the question tool is not available";
        return ask(argsJson);
    }
    if (name == "memory") return memoryTool(argsJson);
    if (name == "notes") return notesTool(argsJson);
    if (name == "search_sessions") return searchSessions(str("query"));
    if (name == "request_history") return requestHistory(i64("limit", 10));
    if (name == "lsp") {
        return lspQuery(cfg, str("operation"), str("path"), i64("line", 1),
                        i64("character", 1));
    }
    if (name == "skill") {
        std::string sn = str("name");
        if (sn.empty()) return "error: the skill name is required";
        if (sn.find('/') != std::string::npos || sn.find("..") != std::string::npos) {
            return "error: bad skill name";
        }
        return readFile(".agent-skills/" + sn + ".md", 1, 2000);
    }
    if (name == "clock") {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
        return buf;
    }
    if (cfg.subagentType == "explore" &&
        (name == "shell" || name == "write_file" || name == "edit_file" ||
         name == "apply_patch")) {
        return "denied: the explore subagent is read-only";
    }
    if (name == "task") {
        if (depth >= cfg.maxDepth) return "error: subagent depth limit reached";
        std::string type = str("subagent_type");
        if (type.empty()) type = "general";
        if (log) log("[agent] subagent: " + str("description"));
        Config sub = cfg;
        sub.subagentType = type;
        sub.maxIterations = std::max(3, cfg.maxIterations - 2);
        std::vector<Message> hist;
        // The subagent's system prompt (the run() hard-codes the default —
        // the subagent only sees the role-scoped prompt through the first
        // user message prefix).
        std::string prompt = str("prompt");
        Result r = run(sub, prompt, hist, confirm, ask, log);
        return r.ok ? r.text : ("subagent error: " + r.error);
        (void)depth;
    }
    // MCP tools: mcp__<server>__<tool>.
    if (name.rfind("mcp__", 0) == 0) {
        for (const auto& srv : cfg.mcpServers) {
            std::string prefix = "mcp__" + srv.name + "__";
            if (name.rfind(prefix, 0) != 0) continue;
            std::string tool = name.substr(prefix.size());
            McpConn conn;
            if (!conn.start(srv)) return conn.error;
            json call = {{"jsonrpc", "2.0"},
                         {"id", conn.nextId++},
                         {"method", "tools/call"},
                         {"params",
                          {{"name", tool},
                           {"arguments", args.is_null() ? json::object() : args}}}};
            json resp = conn.rpc(call);
            if (resp.contains("error")) {
                return "MCP error: " + resp["error"].dump();
            }
            return resp.value("result", json::object())
                .value("content", json::array())
                .dump();
        }
        return "unknown tool: " + name;
    }
    return "unknown tool: " + name;
}

// ---- the SSE streaming ----

struct SseAcc {
    Message msg;
    std::string buf;
    std::string raw;   // the whole response (for the non-SSE JSON fallback)
    bool done = false;
    int64_t promptTokens = 0;
    int64_t completionTokens = 0;
    std::string reasoning;  // the accumulated reasoning_content
};

void feedOpenAiSse(SseAcc& acc, const std::string& chunk,
                   const std::function<void(const std::string&)>& onToken) {
    acc.buf += chunk;
    acc.raw += chunk;
    size_t pos;
    while ((pos = acc.buf.find('\n')) != std::string::npos) {
        std::string line = acc.buf.substr(0, pos);
        acc.buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "data: [DONE]") { acc.done = true; return; }
        if (line.rfind("data: ", 0) != 0) continue;
        json j;
        try {
            j = json::parse(line.substr(6));
        } catch (...) {
            continue;
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            acc.promptTokens = j["usage"].value("prompt_tokens", 0);
            acc.completionTokens = j["usage"].value("completion_tokens", 0);
        }
        auto choices = j.value("choices", json::array());
        if (choices.empty()) continue;
        auto delta = choices[0].value("delta", json::object());
        if (delta.contains("reasoning_content") &&
            delta["reasoning_content"].is_string())
            acc.reasoning += delta["reasoning_content"].get<std::string>();
        if (delta.contains("content") && delta["content"].is_string()) {
            std::string t = delta["content"].get<std::string>();
            if (!t.empty()) {
                acc.msg.content += t;
                if (onToken) onToken(t);
            }
        }
        auto tcs = delta.value("tool_calls", json::array());
        for (const auto& tc : tcs) {
            int idx = tc.value("index", 0);
            if (static_cast<int>(acc.msg.calls.size()) <= idx) {
                acc.msg.calls.resize(static_cast<size_t>(idx + 1));
            }
            auto& tc2 = acc.msg.calls[static_cast<size_t>(idx)];
            if (tc.contains("id") && tc["id"].is_string()) {
                tc2.id += tc["id"].get<std::string>();
            }
            auto fn = tc.value("function", json::object());
            if (fn.contains("name") && fn["name"].is_string()) {
                tc2.name += fn["name"].get<std::string>();
            }
            if (fn.contains("arguments") && fn["arguments"].is_string()) {
                tc2.args += fn["arguments"].get<std::string>();
            }
        }
    }
}

void feedAnthropicSse(SseAcc& acc, const std::string& chunk,
                      const std::function<void(const std::string&)>& onToken) {
    acc.buf += chunk;
    acc.raw += chunk;
    size_t pos;
    while ((pos = acc.buf.find('\n')) != std::string::npos) {
        std::string line = acc.buf.substr(0, pos);
        acc.buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data: ", 0) != 0) continue;
        json j;
        try {
            j = json::parse(line.substr(6));
        } catch (...) {
            continue;
        }
        std::string type = j.value("type", "");
        if (type == "content_block_start") {
            auto cb = j.value("content_block", json::object());
            if (cb.value("type", "") == "tool_use") {
                int idx = j.value("index", 0);
                if (static_cast<int>(acc.msg.calls.size()) <= idx) {
                    acc.msg.calls.resize(static_cast<size_t>(idx + 1));
                }
                auto& tc = acc.msg.calls[static_cast<size_t>(idx)];
                tc.id = cb.value("id", "");
                tc.name = cb.value("name", "");
            }
        } else if (type == "content_block_delta") {
            auto d = j.value("delta", json::object());
            if (d.contains("text") && d["text"].is_string()) {
                std::string t = d["text"].get<std::string>();
                if (!t.empty()) {
                    acc.msg.content += t;
                    if (onToken) onToken(t);
                }
            }
            if (d.contains("partial_json") && d["partial_json"].is_string()) {
                int idx = j.value("index", 0);
                if (static_cast<int>(acc.msg.calls.size()) <= idx) {
                    acc.msg.calls.resize(static_cast<size_t>(idx + 1));
                }
                acc.msg.calls[static_cast<size_t>(idx)].args
                    += d["partial_json"].get<std::string>();
            }
        } else if (type == "message_stop") {
            acc.done = true;
            return;
        }
    }
}

Result run(const Config& cfg, const std::string& prompt,
           std::vector<Message>& history,
           const std::function<int(const std::string& cmd)>& confirm,
           const std::function<std::string(const std::string& questionsJson)>& ask,
           const std::function<void(const std::string&)>& log,
           const std::function<void(const std::string&)>& onToken) {
    // No background process outlives the turn.
    struct Guard { ~Guard() { processCleanupAll(); } } guard;
    (void)guard;
    Result result;
    result.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
    if (!cfg.cwd.empty() && chdir(cfg.cwd.c_str()) != 0 && log) {
        log("cannot chdir to " + cfg.cwd);
    }
    const std::string systemPrompt =
        cfg.subagentType == "explore"
            ? "You are an explore subagent of the matrixcli coding agent. "
              "You search and read the codebase and report findings. You "
              "must NOT write, edit or run the shell — only read, glob, "
              "grep, webfetch and search. Answer with a concise report."
            : cfg.subagentType == "general"
            ? "You are a general subagent of the matrixcli coding agent. "
              "Work on the delegated task and return a concise report."
            : "You are a CLI coding agent running inside the progressive-cli "
              "Matrix client. You help with the local filesystem and the "
              "shell. Use the provided tools for anything that needs the "
              "computer. Prefer small, verifiable steps; keep the todo tool "
              "updated for multi-step work. Answer concisely in the user's "
              "language; only use the tools when they are actually needed. "
              "The shell commands are subject to the user's trust policy.";

    std::string effectiveSystem = systemPrompt;
    if (!cfg.goal.goal.empty() && !cfg.goal.paused) {
        effectiveSystem += "\n\n" + goalContinuationBlock(cfg.goal);
    }
    history.push_back({"user", prompt, {}, "", ""});

    http::Client httpClient;
    httpClient.setTimeout(120);
    if (!cfg.proxy.empty()) {
        auto colon = cfg.proxy.rfind(':');
        if (colon != std::string::npos) {
            http::ProxyConfig pc;
            pc.type = http::ProxyType::SOCKS5;
            pc.host = cfg.proxy.substr(0, colon);
            try { pc.port = std::stoi(cfg.proxy.substr(colon + 1)); } catch (...) {}
            if (pc.enabled()) httpClient.setProxy(pc);
        }
    }

    // Doom-loop protection: 3 identical consecutive tool calls in a row.
    std::string lastTool;
    int repeat = 0;

    // The session-scoped approvals (the "a" answer) and the always-list.
    std::unordered_set<std::string> sessionApproved;
    std::vector<std::string> runAllow = cfg.allowPrefixes;  // + the "A" answers

    // The wrapped confirm: y = once, a = session, A = always.
    auto verdictConfirm = [&](const std::string& cmd) -> int {
        if (sessionApproved.count(cmd)) {
            return static_cast<int>(ConfirmVerdict::Session);
        }
        for (const auto& p : runAllow) {
            if (cmd.rfind(p, 0) == 0) {
                return static_cast<int>(ConfirmVerdict::Always);
            }
        }
        if (!confirm) return static_cast<int>(ConfirmVerdict::Decline);
        int v = confirm(cmd);
        if (v == static_cast<int>(ConfirmVerdict::Session)) {
            sessionApproved.insert(cmd);
        } else if (v == static_cast<int>(ConfirmVerdict::Always)) {
            runAllow.push_back(cmd);
        }
        return v;
    };

    // Start the MCP servers once per run.
    std::vector<std::unique_ptr<McpConn>> mcpConns;
    json mcpSchemas = json::array();
    for (const auto& srv : cfg.mcpServers) {
        auto conn = std::make_unique<McpConn>();
        if (!conn->start(srv)) {
            if (log) log("[agent] MCP: " + conn->error);
            continue;
        }
        json list = {{"jsonrpc", "2.0"}, {"id", conn->nextId++},
                     {"method", "tools/list"}};
        json resp = conn->rpc(list);
        auto tools = resp.value("result", json::object())
                         .value("tools", json::array());
        for (const auto& t : tools) {
            json j = {{"name", "mcp__" + srv.name + "__" + t.value("name", "")},
                      {"description", t.value("description", "")},
                      {"input_schema", t.value("inputSchema", json::object())}};
            mcpSchemas.push_back(j);
        }
        if (log) log("[agent] MCP " + srv.name + ": " +
                     std::to_string(tools.size()) + " tools");
        mcpConns.push_back(std::move(conn));
    }
    json allSchemas = builtinToolsParam();
    for (const auto& j : mcpSchemas) allSchemas.push_back(j);

    for (int iter = 0; iter < cfg.maxIterations; ++iter) {
        result.iterations = iter + 1;
        if (eStopEngaged()) {
            result.error = "paused: the agent e-stop is engaged "
                           "(run 'agent resume' to continue)";
            return result;
        }
        if (!matrixcli::g_interrupted.load()) {
            result.error = "interrupted (Ctrl+C)";
            return result;
        }
        if (matrixcli::g_agentInterrupt.load()) {
            result.error = "interrupted (Esc)";
            return result;
        }
        json reqBody;
        http::Request req;
        if (cfg.provider == "anthropic") {
            json messages = json::array();
            buildAnthropicMessages(history, effectiveSystem, messages);
            reqBody = {{"model", cfg.model},
                       {"system", systemPrompt},
                       {"messages", messages},
                       {"tools", allSchemas},
                       {"max_tokens", 4096}};
            if (!cfg.reasoning.empty()) {
                int budget = cfg.reasoning == "low" ? 1024
                           : cfg.reasoning == "medium" ? 2048 : 4096;
                reqBody["thinking"] = {{"type", "enabled"},
                                       {"budget_tokens", budget}};
            }
            req.url = anthropicUrl(cfg);
            req.headers["x-api-key"] = cfg.key;
            req.headers["anthropic-version"] = "2023-06-01";
            req.headers["content-type"] = "application/json";
        } else {
            json messages = json::array();
            buildOpenAiMessages(history, effectiveSystem, messages);
            reqBody = {{"model", cfg.model},
                       {"messages", messages},
                       {"tools", openAiToolsParam(allSchemas)}};
            if (!cfg.reasoning.empty()) {
                reqBody["reasoning_effort"] = cfg.reasoning;
            }
            req.url = openAiUrl(cfg);
            req.headers["authorization"] = "Bearer " + cfg.key;
            req.headers["content-type"] = "application/json";
        }
        req.method = "POST";
        req.body = reqBody.dump();
        if (log) log("[agent] iteration " + std::to_string(iter + 1)
                      + ": calling " + cfg.model + " ...");
        Message assistant;
        assistant.role = "assistant";
        bool haveAssistant = false;
        if (onToken) {
            // The SSE streaming: the tokens appear as they arrive; the
            // tool-call fragments are merged from the deltas.
            json sBody = reqBody;
            if (cfg.provider != "anthropic") sBody["stream"] = true;
            SseAcc acc;
            acc.msg.role = "assistant";
            http::Response sresp = httpClient.streamPost(
                req.url, sBody.dump(), req.headers,
                [&](const std::string& chunk) {
                    if (cfg.provider == "anthropic") {
                        feedAnthropicSse(acc, chunk, onToken);
                    } else {
                        feedOpenAiSse(acc, chunk, onToken);
                    }
                },
                [&]() {
                    return matrixcli::g_agentInterrupt.load() ||
                           !matrixcli::g_interrupted.load();
                });
            if (!matrixcli::g_interrupted.load()) {
                result.error = "interrupted (Ctrl+C)";
                return result;
            }
            if (matrixcli::g_agentInterrupt.load()) {
                result.error = "interrupted (Esc)";
                return result;
            }
            if (sresp.ok() && acc.done &&
                (!acc.msg.content.empty() || !acc.msg.calls.empty())) {
                // Only the COMPLETE stream (the [DONE] seen) counts; a cut
                // stream falls back to the plain request below.
                assistant = acc.msg;
                haveAssistant = true;
                result.streamed = true;
                result.promptTokens += acc.promptTokens;
                result.completionTokens += acc.completionTokens;
                result.reasoning += acc.reasoning;
                agentAddUsage(acc.promptTokens, acc.completionTokens, cfg.model);
            } else if (sresp.ok() && !acc.raw.empty()) {
                // The provider answered with a plain JSON (not the SSE):
                // parse the whole body instead of re-requesting.
                try {
                    json plain = json::parse(acc.raw);
                    Message m2 = cfg.provider == "anthropic"
                                     ? parseAnthropicResponse(plain)
                                     : parseOpenAiResponse(plain);
                    if (!m2.content.empty() || !m2.calls.empty()) {
                        assistant = m2;
                        haveAssistant = true;
                        result.streamed = false;
                    }
                } catch (...) {}
            }
        }
        if (!haveAssistant) {
            http::Response resp;
            int attempts = 0;
            for (; attempts < 3; ++attempts) {
                resp = httpClient.doRequest(req);
                if (resp.ok()) break;
                // The agora retry semantics: back off on rate limits and
                // server errors, give up on the 4xx ones.
                if (!resp.server_error() && resp.status_code != 429) break;
                if (attempts < 2) {
                    if (log) log("[agent] retrying in " +
                                 std::to_string(1 << attempts) + "s ...");
                    sleep(1 << attempts);
                }
            }
            if (!resp.ok()) {
                result.error = "HTTP " + std::to_string(resp.status_code) + ": "
                             + (resp.body.size() > 300 ? resp.body.substr(0, 300)
                                                       : resp.body);
                return result;
            }
            json parsed;
            try {
                parsed = json::parse(resp.body);
            } catch (...) {
                result.error = "bad JSON from the LLM: "
                             + resp.body.substr(0, 200);
                return result;
            }
            assistant = cfg.provider == "anthropic"
                            ? parseAnthropicResponse(parsed)
                            : parseOpenAiResponse(parsed);
            if (parsed.contains("usage") && parsed["usage"].is_object()) {
                result.promptTokens += parsed["usage"].value("prompt_tokens", 0);
                result.completionTokens +=
                    parsed["usage"].value("completion_tokens", 0);
            }
            if (cfg.provider != "anthropic" &&
                parsed.value("choices", json::array()).size() > 0) {
                auto msg = parsed["choices"][0].value("message", json::object());
                if (msg.contains("reasoning_content") &&
                    msg["reasoning_content"].is_string())
                    result.reasoning += msg["reasoning_content"].get<std::string>();
            }
            agentAddUsage(static_cast<int>(req.body.size() / 4),
                          static_cast<int>(resp.body.size() / 4), cfg.model);
        }
        if (assistant.calls.empty()) {
            history.push_back(assistant);
            result.ok = true;
            result.text = assistant.content;
            if (result.streamed && onToken) onToken("\n");
            return result;
        }
        history.push_back(assistant);
        // The auto-compaction: the history beyond the threshold gets the
        // LLM summary before the next request.
        if (cfg.compactThreshold > 0) {
            int64_t chars = 0;
            for (const auto& m : history) chars += m.content.size();
            int ctx = contextSizeFor(cfg.model);
            int64_t usedTokens = chars / 4;
            if (ctx > 0 &&
                usedTokens * 100 >= static_cast<int64_t>(ctx) *
                                       cfg.compactThreshold) {
                std::string oldText;
                int keep = 6;
                int n = static_cast<int>(history.size());
                for (int i = 0; i < n - keep && i < n; ++i) {
                    if (!history[static_cast<size_t>(i)].content.empty()) {
                        oldText += history[static_cast<size_t>(i)].role + ": "
                                 + history[static_cast<size_t>(i)]
                                       .content.substr(0, 1500) + "\n";
                    }
                }
                if (log) log("[agent] auto-compacting ...");
                std::string summary = llmSimple(
                    cfg,
                    "Summarise this conversation so far into a few dense "
                    "lines (the key decisions, the state, the open "
                    "questions):\n\n" + oldText);
                std::vector<Message> tail(
                    history.end() - std::min<int>(keep, n), history.end());
                history.clear();
                if (!summary.empty()) {
                    history.push_back({"user",
                                       "[CONTEXT SUMMARY]: " + summary,
                                       {}, "", ""});
                }
                for (auto& m : tail) history.push_back(m);
            }
        }
        auto isReadOnly = [](const std::string& n) {
            return n == "read_file" || n == "glob" || n == "grep" ||
                   n == "webfetch" || n == "clock" || n == "search_sessions" ||
                   n == "request_history" || n == "skill" || n == "lsp";
        };
        // The read-only calls run in parallel, the mutating ones
        // sequentially (in the order).
        std::vector<std::string> results(assistant.calls.size());
        std::vector<std::thread> workers;
        for (size_t ci = 0; ci < assistant.calls.size(); ++ci) {
            if (isReadOnly(assistant.calls[ci].name)) {
                workers.emplace_back([&, ci]() {
                    results[ci] = executeTool(cfg, assistant.calls[ci].name,
                                              assistant.calls[ci].args,
                                              verdictConfirm, ask, log, 0);
                });
            }
        }
        for (auto& w : workers) w.join();
        for (size_t ci = 0; ci < assistant.calls.size(); ++ci) {
            const auto& c = assistant.calls[static_cast<size_t>(ci)];
            // The doom-loop check: the same tool+args three times in a
            // row is a stuck agent — fail the call so the model moves on.
            std::string key = c.name + " " + c.args;
            if (key == lastTool) {
                if (++repeat >= 3) {
                    history.push_back({"tool",
                                       "refused: this exact call was already "
                                       "made 3 times — try a different "
                                       "approach", {}, c.id, c.name});
                    continue;
                }
            } else {
                lastTool = key;
                repeat = 1;
            }
            if (log) log("[agent] tool: " + c.name + " " + c.args);
            if (c.name == "shell" && log) {
                try {
                    log("[agent] ⚙ running: "
                        + nlohmann::json::parse(c.args).value("command", ""));
                } catch (...) {}
            }
            std::string out = isReadOnly(c.name)
                ? results[static_cast<size_t>(ci)]
                : executeTool(cfg, c.name, c.args, verdictConfirm, ask, log, 0);
            history.push_back({"tool", out, {}, c.id, c.name});
            if (log) {
                std::string preview = out.size() > 200 ? out.substr(0, 200) + "..."
                                                       : out;
                log("  -> " + preview);
            }
        }
    }
    result.error = "max iterations reached (" + std::to_string(cfg.maxIterations) + ")";
    return result;
}

}} // namespace matrixcli::agenttools

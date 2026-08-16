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

std::string openAiUrl(const Config& cfg) {
    return baseEndpoint(cfg) + "/v1/chat/completions";
}

std::string anthropicUrl(const Config& cfg) {
    return baseEndpoint(cfg) + "/v1/messages";
}

json builtinToolsParam() {
    return json::parse(toolSchemasJson());
}

json openAiToolsParam(const json& src) {
    json tools = json::array();
    for (const auto& t : src) {
        json j = {{"type", "function"},
                  {"function",
                   {{"name", t["name"]},
                    {"description", t["description"]},
                    {"parameters", t["input_schema"]}}}};
        tools.push_back(j);
    }
    return tools;
}

void buildOpenAiMessages(const std::vector<Message>& history,
                         const std::string& systemPrompt, json& out) {
    if (!systemPrompt.empty()) {
        out.push_back({{"role", "system"}, {"content", systemPrompt}});
    }
    for (const auto& m : history) {
        if (m.role == "tool") {
            out.push_back({{"role", "tool"},
                           {"tool_call_id", m.toolCallId},
                           {"content", m.content}});
        } else if (m.role == "assistant") {
            json j = {{"role", "assistant"}, {"content", m.content}};
            if (!m.calls.empty()) {
                json tcs = json::array();
                for (const auto& c : m.calls) {
                    tcs.push_back({{"id", c.id},
                                   {"type", "function"},
                                   {"function",
                                    {{"name", c.name}, {"arguments", c.args}}}});
                }
                j["tool_calls"] = tcs;
            }
            out.push_back(j);
        } else {
            out.push_back({{"role", m.role}, {"content", m.content}});
        }
    }
}

void buildAnthropicMessages(const std::vector<Message>& history,
                            const std::string& /*system*/, json& out) {
    for (const auto& m : history) {
        if (m.role == "tool") {
            json j = {{"role", "user"},
                      {"content", json::array(
                                     {{{"type", "tool_result"},
                                       {"tool_use_id", m.toolCallId},
                                       {"content", m.content}}})}};
            out.push_back(j);
        } else if (m.role == "assistant") {
            json blocks = json::array();
            if (!m.content.empty()) {
                blocks.push_back({{"type", "text"}, {"text", m.content}});
            }
            for (const auto& c : m.calls) {
                json args = json::object();
                if (!c.args.empty()) {
                    try {
                        args = json::parse(c.args);
                    } catch (...) {
                        args = {{"raw", c.args}};
                    }
                }
                blocks.push_back(
                    {{"type", "tool_use"}, {"id", c.id}, {"name", c.name},
                     {"input", args}});
            }
            out.push_back({{"role", "assistant"}, {"content", blocks}});
        } else {
            out.push_back({{"role", m.role},
                           {"content", json::array(
                                          {{{"type", "text"}, {"text", m.content}}})}});
        }
    }
}

Message parseOpenAiResponse(const json& resp) {
    Message m;
    m.role = "assistant";
    auto choice = resp.value("choices", json::array());
    if (choice.empty() || !choice[0].is_object()) return m;
    auto msg = choice[0].value("message", json::object());
    if (msg.contains("content") && msg["content"].is_string())
        m.content = msg["content"].get<std::string>();
    auto tcs = msg.value("tool_calls", json::array());
    for (const auto& t : tcs) {
        if (!t.is_object()) continue;
        auto fn = t.value("function", json::object());
        ToolCall c;
        c.id = t.value("id", "");
        c.name = fn.value("name", "");
        if (fn.contains("arguments") && fn["arguments"].is_string())
            c.args = fn["arguments"].get<std::string>();
        m.calls.push_back(c);
    }
    return m;
}

Message parseAnthropicResponse(const json& resp) {
    Message m;
    m.role = "assistant";
    auto blocks = resp.value("content", json::array());
    for (const auto& b : blocks) {
        std::string type = b.value("type", "");
        if (type == "text") {
            m.content += b.value("text", "");
        } else if (type == "tool_use") {
            ToolCall c;
            c.id = b.value("id", "");
            c.name = b.value("name", "");
            c.args = b.value("input", json::object()).dump();
            m.calls.push_back(c);
        }
    }
    return m;
}

// The plain single LLM call (no tools) — used by the auto-compaction.
std::vector<ProviderPreset> providerPresets() {
    std::vector<ProviderPreset> out;
    for (const auto& p : util::llmPresets()) {
        out.push_back({p.name, p.provider, p.endpoint, p.model});
    }
    return out;
}

bool applyProviderPreset(Config& cfg, const std::string& name) {
    const auto* p = util::findLlmPreset(name);
    if (!p) return false;
    cfg.provider = p->provider;
    cfg.endpoint = p->endpoint;
    cfg.model = p->model;
    return true;
}

int contextSizeFor(const std::string& model) {    if (model.find("claude-3-5") != std::string::npos ||
        model.find("claude-3-7") != std::string::npos ||
        model.find("claude-3-opus") != std::string::npos ||
        model.find("o3") != std::string::npos || model.find("o4") != std::string::npos)
        return 200000;
    if (model.find("gpt-4") != std::string::npos) return 128000;
    if (model.find("deepseek") != std::string::npos) return 64000;
    if (model.find("llama") != std::string::npos) return 128000;
    if (model.find("qwen") != std::string::npos) return 32000;
    if (model.find("claude") != std::string::npos) return 200000;
    return 128000;
}

static double pricePerM(const std::string& model, bool output) {
    if (model.find("gpt-4o-mini") != std::string::npos) return output ? 0.60 : 0.15;
    if (model.find("gpt-4o") != std::string::npos) return output ? 10.0 : 2.5;
    if (model.find("gpt-4.1") != std::string::npos) return output ? 8.0 : 2.0;
    if (model.find("o3") != std::string::npos || model.find("o4") != std::string::npos)
        return output ? 12.0 : 2.0;
    if (model.find("claude-3-5-haiku") != std::string::npos)
        return output ? 4.0 : 0.8;
    if (model.find("claude-3-5-sonnet") != std::string::npos ||
        model.find("claude-3-7") != std::string::npos)
        return output ? 15.0 : 3.0;
    if (model.find("claude-3-opus") != std::string::npos)
        return output ? 75.0 : 15.0;
    if (model.find("claude-3-haiku") != std::string::npos)
        return output ? 1.25 : 0.25;
    if (model.find("deepseek") != std::string::npos) return output ? 1.1 : 0.27;
    if (model.find("llama") != std::string::npos ||
        model.find("qwen") != std::string::npos)
        return 0.0;
    return -1.0;
}

double estimateCost(const std::string& model, int64_t inputTokens,
                    int64_t outputTokens) {
    double inP = pricePerM(model, false);
    double outP = pricePerM(model, true);
    double cost = 0.0;
    if (inP >= 0) cost += inP * static_cast<double>(inputTokens) / 1000000.0;
    if (outP >= 0) cost += outP * static_cast<double>(outputTokens) / 1000000.0;
    return cost;
}

int contextSizeForModel(const std::string& model) {
    return contextSizeFor(model);
}

static std::mutex g_usageMutex;
static int64_t g_inTokens = 0;
static int64_t g_outTokens = 0;
static std::string g_lastModel;

void agentAddUsage(int inputTokens, int outputTokens, const std::string& model) {
    std::lock_guard<std::mutex> lk(g_usageMutex);
    g_inTokens += inputTokens;
    g_outTokens += outputTokens;
    if (!model.empty()) g_lastModel = model;
}

std::string agentUsageLine() {
    std::lock_guard<std::mutex> lk(g_usageMutex);
    double inM = static_cast<double>(g_inTokens) / 1000000.0;
    double outM = static_cast<double>(g_outTokens) / 1000000.0;
    double inP = pricePerM(g_lastModel, false);
    double outP = pricePerM(g_lastModel, true);
    std::ostringstream ss;
    ss << g_inTokens << " in / " << g_outTokens << " out tokens";
    // The % of the context window (the rough chars/4 estimate).
    int ctx = contextSizeFor(g_lastModel);
    double used = static_cast<double>(g_inTokens + g_outTokens);
    ss << " · " << std::fixed << std::setprecision(1)
       << (ctx > 0 ? used * 100.0 / ctx : 0.0) << "% of the "
       << ctx / 1000 << "k context";
    if (inP >= 0) {
        double cost = inM * inP + outM * outP;
        ss << " · $" << std::fixed << std::setprecision(3) << cost;
    } else {
        ss << " · price unknown";
    }
    return ss.str();
}

int undoLastTurns(std::vector<Message>& history, int n) {
    if (n < 1) n = 1;
    int userSeen = 0;
    int cut = static_cast<int>(history.size());
    for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
        if (history[static_cast<size_t>(i)].role == "user") {
            if (++userSeen >= n) { cut = i; break; }
        }
    }
    if (userSeen < n) cut = 0;
    int removed = static_cast<int>(history.size()) - cut;
    if (cut < static_cast<int>(history.size())) {
        history.resize(static_cast<size_t>(cut));
    }
    return removed;
}

// ---- the LSP client (the clangd via the Content-Length framing) ----

struct LspConn {
    FILE* f = nullptr;
    int nextId = 1;
    std::string root;

    bool start(const std::string& cmd) {
        f = popen(cmd.c_str(), "r+");
        if (!f) return false;
        json init = {{"jsonrpc", "2.0"}, {"id", nextId++}, {"method", "initialize"},
                     {"params",
                      {{"processId", static_cast<int>(getpid())},
                       {"rootUri", "file://" + root},
                       {"capabilities", json::object()},
                       {"workspaceFolders",
                        json::array({{{"uri", "file://" + root}, {"name", "root"}}})}}}};
        json resp = rpc(init);
        if (!resp.contains("result")) return false;
        notify({"jsonrpc", "2.0"}, "initialized", json::object());
        return true;
    }

    bool notify(const json& base, const std::string& method,
                const json& params) {
        json n = base;
        n["method"] = method;
        n["params"] = params;
        std::string body = n.dump();
        std::string framed = "Content-Length: " + std::to_string(body.size())
                           + "\r\n\r\n" + body;
        if (!f || fwrite(framed.data(), 1, framed.size(), f) != framed.size()) {
            return false;
        }
        fflush(f);
        return true;
    }

    json rpc(const json& req) {
        std::string body = req.dump();
        std::string framed = "Content-Length: " + std::to_string(body.size())
                           + "\r\n\r\n" + body;
        if (!f || fwrite(framed.data(), 1, framed.size(), f) != framed.size()) {
            return json();
        }
        fflush(f);
        // Read one framed response.
        char buf[65536];
        std::string acc;
        while (fgets(buf, sizeof(buf), f)) {
            acc += buf;
            auto hdrEnd = acc.find("\r\n\r\n");
            if (hdrEnd == std::string::npos) continue;
            auto cl = acc.find("Content-Length:");
            if (cl == std::string::npos) continue;
            int len = std::atoi(acc.c_str() + cl + 15);
            size_t bodyStart = hdrEnd + 4;
            while (acc.size() < bodyStart + static_cast<size_t>(len)) {
                if (!fgets(buf, sizeof(buf), f)) return json();
                acc += buf;
            }
            try {
                return json::parse(acc.substr(bodyStart,
                                              static_cast<size_t>(len)));
            } catch (...) {
                return json();
            }
        }
        return json();
    }

    ~LspConn() {
        if (f) pclose(f);
    }
};

std::string lspQueryPublic(const Config& cfg, const std::string& operation,
                           const std::string& path, int line, int character) {
    return lspQuery(cfg, operation, path, line, character);
}

std::string lspQuery(const Config& cfg, const std::string& operation,
                   const std::string& path, int line, int character) {
    if (path.empty()) return "error: the path is required";
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return "error: getcwd failed";
    std::string abs = path;
    if (abs[0] != '/') abs = std::string(cwd) + "/" + abs;
    LspConn conn;
    conn.root = cwd;
    std::string cmd = "clangd --background-index=0 --limit-results=5";
    if (!conn.start(cmd)) {
        return "error: clangd is not available (install clangd for the LSP)";
    }
    std::string uri = "file://" + abs;
    json req = {{"jsonrpc", "2.0"}, {"id", conn.nextId++},
                {"method", "textDocument/" + operation},
                {"params",
                 {{"textDocument", {{"uri", uri}}},
                  {"position",
                   {{"line", std::max(0, line - 1)},
                    {"character", std::max(0, character - 1)}}}}}};
    json resp = conn.rpc(req);
    if (!resp.contains("result") || resp["result"].is_null()) {
        return "(no result from the language server)";
    }
    if (operation == "hover") {
        json contents = resp["result"].value("contents", json());
        std::string text;
        if (contents.is_string()) {
            text = contents.get<std::string>();
        } else if (contents.is_object() && contents.contains("value")) {
            text = contents["value"].get<std::string>();
        } else if (contents.is_array() && !contents.empty()) {
            text = contents[0].dump();
        }
        // Strip the markdown a bit for the terminal.
        std::string out;
        bool inCode = false;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '`') { inCode = !inCode; continue; }
            if (!inCode && (text[i] == '*' || text[i] == '#')) continue;
            out += text[i];
        }
        return out.empty() ? "(no hover info)" : out;
    }
    if (operation == "definition") {
        json res = resp["result"];
        json loc = res.is_array() && !res.empty() ? res[0] : res;
        std::string tUri = loc.value("uri", "");
        int ln = loc.value("range", json::object())
                     .value("start", json::object())
                     .value("line", 0);
        int ch = loc.value("range", json::object())
                     .value("start", json::object())
                     .value("character", 0);
        std::string where = tUri;
        if (where.rfind("file://", 0) == 0) where = where.substr(7);
        return where + ":" + std::to_string(ln + 1) + ":" + std::to_string(ch + 1);
    }
    return "(unsupported operation)";
}

}} // namespace matrixcli::agenttools

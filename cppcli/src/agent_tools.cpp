#include "agent_tools.hpp"

#include "../lib/http/http.hpp"
#include "../lib/json/json.hpp"

#include <fstream>
#include <glob.h>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace matrixcli { namespace agenttools {

using nlohmann::json;

namespace {

std::string baseEndpoint(const Config& cfg) {
    if (!cfg.endpoint.empty()) {
        std::string e = cfg.endpoint;
        while (!e.empty() && e.back() == '/') e.pop_back();
        return e;
    }
    return cfg.provider == "anthropic" ? "https://api.anthropic.com"
                                       : "https://api.openai.com";
}

std::string toolSchemasJson() {
    json out = json::array();
    auto schema = [&](const char* name, const char* desc,
                      const json& props, const json& required) {
        out.push_back({{"name", name},
                       {"description", desc},
                       {"input_schema",
                        {{"type", "object"}, {"properties", props},
                         {"required", required}}}});
    };
    schema("shell",
           "Run a shell command in the working directory (subject to the "
           "user's trust policy). Returns stdout+stderr.",
           {{"command", {{"type", "string"},
                         {"description", "the shell command to run"}}}},
           {"command"});
    schema("read_file",
           "Read a text file's contents (fails on binary files).",
           {{"path", {{"type", "string"}}}}, {"path"});
    schema("write_file",
           "Create or overwrite a text file with the given content.",
           {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}},
           {"path", "content"});
    schema("edit_file",
           "Replace an exact string occurrence in a file (fails if it is "
           "missing or appears more than once).",
           {{"path", {{"type", "string"}}},
            {"old", {{"type", "string"}}},
            {"new", {{"type", "string"}}}},
           {"path", "old", "new"});
    schema("glob",
           "List files matching a glob pattern (e.g. src/**/*.cpp).",
           {{"pattern", {{"type", "string"}}}}, {"pattern"});
    schema("grep",
           "Search file contents with a regular expression under a path.",
           {{"pattern", {{"type", "string"}}},
            {"path", {{"type", "string"}}}},
           {"pattern"});
    schema("webfetch",
           "Fetch a URL and return its text content.",
           {{"url", {{"type", "string"}}}}, {"url"});
    return out.dump();
}

// ---- the tools ----

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "error: cannot open " + path;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() > 60000) s = s.substr(0, 60000) + "\n...(truncated)";
    if (!s.empty() && s[0] == '\0') return "error: binary file";
    return s.empty() ? "(empty file)" : s;
}

std::string writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return "error: cannot write " + path;
    f << content;
    return "wrote " + std::to_string(content.size()) + " bytes to " + path;
}

std::string editFile(const std::string& path, const std::string& oldText,
                     const std::string& newText) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "error: cannot open " + path;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    size_t pos = s.find(oldText);
    if (pos == std::string::npos) return "error: old text not found";
    if (s.find(oldText, pos + 1) != std::string::npos) {
        return "error: old text appears more than once";
    }
    s.replace(pos, oldText.size(), newText);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return "error: cannot write " + path;
    out << s;
    return "edited " + path;
}

std::string globFiles(const std::string& pattern) {
    glob_t g{};
    if (glob(pattern.c_str(), GLOB_TILDE, nullptr, &g) != 0) {
        globfree(&g);
        return "(no matches)";
    }
    std::string out;
    int shown = 0;
    for (size_t i = 0; i < g.gl_pathc && shown < 200; ++i, ++shown) {
        out += std::string(g.gl_pathv[i]) + "\n";
    }
    globfree(&g);
    return out.empty() ? "(no matches)" : out;
}

std::string grepFiles(const std::string& pattern, const std::string& path) {
    std::regex re;
    try {
        re = std::regex(pattern);
    } catch (...) {
        return "error: bad regex";
    }
    glob_t g{};
    std::string globPat = path;
    if (globPat.empty()) globPat = "*";
    if (glob(globPat.c_str(), GLOB_TILDE, nullptr, &g) != 0) {
        globfree(&g);
        return "(no files)";
    }
    std::string out;
    int hits = 0;
    for (size_t i = 0; i < g.gl_pathc && hits < 100; ++i) {
        std::ifstream f(g.gl_pathv[i], std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string body = ss.str();
        if (body.size() > 200000) continue;
        std::istringstream lines(body);
        std::string line;
        int ln = 0;
        while (std::getline(lines, line) && hits < 100) {
            ln++;
            if (std::regex_search(line, re)) {
                out += std::string(g.gl_pathv[i]) + ":" + std::to_string(ln)
                     + ": " + line + "\n";
                hits++;
            }
        }
    }
    globfree(&g);
    return out.empty() ? "(no matches)" : out;
}

std::string shellCmd(const std::string& cmd) {
    std::string full = cmd + " 2>&1";
    FILE* f = popen(full.c_str(), "r");
    if (!f) return "error: popen failed";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    int rc = pclose(f);
    if (out.size() > 20000) out = out.substr(0, 20000) + "\n...(truncated)";
    if (rc != 0) out += "\n[exit " + std::to_string(rc) + "]";
    return out.empty() ? "(no output)" : out;
}

std::string webFetch(const std::string& url) {
    http::Client c;
    c.setTimeout(20);
    auto r = c.get(url);
    if (!r.ok()) return "error: HTTP " + std::to_string(r.status_code);
    if (r.body.size() > 30000) r.body = r.body.substr(0, 30000) + "\n...(truncated)";
    return r.body;
}

// The trust policy for the shell: deny > allow > level.
enum class Verdict { Allow, Ask, Deny };

Verdict checkTrust(const Config& cfg, const std::string& cmd) {
    for (const auto& p : cfg.denyPrefixes) {
        if (cmd.rfind(p, 0) == 0) return Verdict::Deny;
    }
    for (const auto& p : cfg.allowPrefixes) {
        if (cmd.rfind(p, 0) == 0) return Verdict::Allow;
    }
    if (cfg.trust == "allow") return Verdict::Allow;
    if (cfg.trust == "deny") return Verdict::Deny;
    return Verdict::Ask;
}

// ---- the LLM adapters ----

std::string openAiUrl(const Config& cfg) {
    return baseEndpoint(cfg) + "/v1/chat/completions";
}

std::string anthropicUrl(const Config& cfg) {
    return baseEndpoint(cfg) + "/v1/messages";
}

json openAiToolsParam() {
    json tools = json::array();
    json src = json::parse(toolSchemasJson());
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

json anthropicToolsParam() {
    return json::parse(toolSchemasJson());
}

// Convert the internal history to the provider's message list.
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
    if (choice.empty()) return m;
    auto msg = choice[0].value("message", json::object());
    m.content = msg.value("content", "");
    auto tcs = msg.value("tool_calls", json::array());
    for (const auto& t : tcs) {
        auto fn = t.value("function", json::object());
        ToolCall c;
        c.id = t.value("id", "");
        c.name = fn.value("name", "");
        c.args = fn.value("arguments", "");
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

} // namespace

std::string executeTool(const Config& cfg, const std::string& name,
                        const std::string& argsJson,
                        const std::function<bool(const std::string&)>& confirm) {
    json args = json::object();
    if (!argsJson.empty()) {
        try {
            args = json::parse(argsJson);
        } catch (...) {
            return "error: bad arguments JSON";
        }
    }
    auto str = [&](const char* k) {
        return args.value(k, "");
    };
    if (name == "shell") {
        std::string cmd = str("command");
        if (cmd.empty()) return "error: empty command";
        Verdict v = checkTrust(cfg, cmd);
        if (v == Verdict::Deny) return "denied by the trust policy: " + cmd;
        if (v == Verdict::Ask && confirm) {
            if (!confirm(cmd)) return "declined by the user: " + cmd;
        }
        return shellCmd(cmd);
    }
    if (name == "read_file") return readFile(str("path"));
    if (name == "write_file") return writeFile(str("path"), str("content"));
    if (name == "edit_file") {
        return editFile(str("path"), str("old"), str("new"));
    }
    if (name == "glob") return globFiles(str("pattern"));
    if (name == "grep") return grepFiles(str("pattern"), str("path"));
    if (name == "webfetch") return webFetch(str("url"));
    return "unknown tool: " + name;
}

Result run(const Config& cfg, const std::string& prompt,
           std::vector<Message>& history,
           const std::function<bool(const std::string&)>& confirm,
           const std::function<void(const std::string&)>& log) {
    Result result;
    if (!cfg.cwd.empty() && chdir(cfg.cwd.c_str()) != 0 && log) {
        log("cannot chdir to " + cfg.cwd);
    }
    const std::string systemPrompt =
        "You are a CLI coding agent running inside the progressive-cli "
        "Matrix client. You help with the local filesystem and the shell. "
        "Use the provided tools for anything that needs the computer. "
        "Prefer small, verifiable steps. Answer concisely in the user's "
        "language; only use the tools when they are actually needed. The "
        "shell commands are subject to the user's trust policy.";

    history.push_back({"user", prompt, {}, "", ""});

    http::Client httpClient;
    httpClient.setTimeout(120);

    for (int iter = 0; iter < cfg.maxIterations; ++iter) {
        result.iterations = iter + 1;
        json reqBody;
        http::Request req;
        if (cfg.provider == "anthropic") {
            json messages = json::array();
            buildAnthropicMessages(history, systemPrompt, messages);
            reqBody = {{"model", cfg.model},
                       {"system", systemPrompt},
                       {"messages", messages},
                       {"tools", anthropicToolsParam()},
                       {"max_tokens", 4096}};
            req.url = anthropicUrl(cfg);
            req.headers["x-api-key"] = cfg.key;
            req.headers["anthropic-version"] = "2023-06-01";
            req.headers["content-type"] = "application/json";
        } else {
            json messages = json::array();
            buildOpenAiMessages(history, systemPrompt, messages);
            reqBody = {{"model", cfg.model},
                       {"messages", messages},
                       {"tools", openAiToolsParam()}};
            req.url = openAiUrl(cfg);
            req.headers["authorization"] = "Bearer " + cfg.key;
            req.headers["content-type"] = "application/json";
        }
        req.method = "POST";
        req.body = reqBody.dump();
        if (log) log("[agent] iteration " + std::to_string(iter + 1)
                      + ": calling " + cfg.model + " ...");
        http::Response resp = httpClient.doRequest(req);
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
        Message assistant = cfg.provider == "anthropic"
                                ? parseAnthropicResponse(parsed)
                                : parseOpenAiResponse(parsed);
        if (assistant.calls.empty()) {
            history.push_back(assistant);
            result.ok = true;
            result.text = assistant.content;
            return result;
        }
        // Execute the tool calls, feed the results back.
        history.push_back(assistant);
        for (const auto& c : assistant.calls) {
            if (log) log("[agent] tool: " + c.name + " " + c.args);
            std::string out = executeTool(cfg, c.name, c.args, confirm);
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

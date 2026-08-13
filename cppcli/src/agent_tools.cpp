#include "agent_tools.hpp"

#include "../lib/http/http.hpp"
#include "../lib/json/json.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <glob.h>
#include <map>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace matrixcli { namespace agenttools {

using nlohmann::json;

namespace {

// The session's todo list (the todowrite/update_plan tool).
std::vector<std::pair<std::string, std::string>> g_todos;  // {status, content}

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
           "user's trust policy). Returns stdout+stderr; the output is "
           "truncated to ~20KB with a hint.",
           {{"command", {{"type", "string"},
                         {"description", "the shell command to run"}}},
            {"timeout", {{"type", "integer"},
                         {"description", "kill after this many seconds (default 60)"}}},
            {"workdir", {{"type", "string"},
                         {"description", "run in this directory instead of the cwd"}}}},
           {"command"});
    schema("read_file",
           "Read a text file's contents with line numbers (fails on binary "
           "files); use offset/limit to page through long files.",
           {{"path", {{"type", "string"}}},
            {"offset", {{"type", "integer"},
                        {"description", "1-indexed first line (default 1)"}}},
            {"limit", {{"type", "integer"},
                       {"description", "max lines to return (default 2000)"}}}},
           {"path"});
    schema("write_file",
           "Create or overwrite a text file with the given content.",
           {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}},
           {"path", "content"});
    schema("edit_file",
           "Replace an exact string occurrence in a file (fails if it is "
           "missing or appears more than once, unless replaceAll is true).",
           {{"path", {{"type", "string"}}},
            {"old", {{"type", "string"}}},
            {"new", {{"type", "string"}}},
            {"replaceAll", {{"type", "boolean"}}}},
           {"path", "old", "new"});
    schema("apply_patch",
           "Apply a multi-file patch. Format:\n"
           "*** Begin Patch\n*** Add File: path/to/file\n<contents>\n"
           "*** End of File\n*** Update File: path/to/file\n*** Begin Change\n"
           "<old lines>\n*** End Change\n*** Begin Replacement\n<new lines>\n"
           "*** End Replacement\n*** Delete File: path/to/file\n*** End Patch",
           {{"patch_text", {{"type", "string"}}}}, {"patch_text"});
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
    schema("todo",
           "Track the plan: replaces the current task list. Exactly one "
           "task should be in_progress at a time.",
           {{"todos",
             {{"type", "array"},
              {"items",
               {{"type", "object"},
                {"properties",
                 {{"content", {{"type", "string"}}},
                  {"status",
                   {{"type", "string"},
                    {"enum", {"pending", "in_progress", "completed",
                              "cancelled"}}}}},
                 {"required", {"content", "status"}}}}}}}},
           {"todos"});
    schema("question",
           "Ask the user questions and return their answers as the tool "
           "result (the loop continues afterwards).",
           {{"questions",
             {{"type", "array"},
              {"items",
               {{"type", "object"},
                {"properties",
                 {{"question", {{"type", "string"}}},
                  {"header", {{"type", "string"}}},
                  {"options",
                   {{"type", "array"},
                    {"items",
                     {{"type", "object"},
                      {"properties",
                       {{"label", {{"type", "string"}}},
                        {"description", {{"type", "string"}}}}}}}}},
                  {"multiple", {{"type", "boolean"}}}}}}}}}},
           {"questions"});
    schema("clock",
           "The current date and time (UTC).",
           json::object(), json::array());
    return out.dump();
}

// ---- output truncation ----

std::string truncateOut(std::string s, size_t bytes = 20000,
                        size_t lines = 2000) {
    if (s.size() > bytes) {
        s = s.substr(0, bytes);
        s += "\n...(truncated, use read_file with offset/limit or grep to "
             "dig deeper)";
    }
    size_t nl = std::count(s.begin(), s.end(), '\n');
    if (nl > lines) {
        std::istringstream in(s);
        std::string line, out;
        for (size_t i = 0; i < lines && std::getline(in, line); ++i) {
            out += line + "\n";
        }
        s = out + "...(truncated)";
    }
    return s;
}

// ---- the tools ----

std::string readFile(const std::string& path, int offset, int limit) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "error: cannot open " + path;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() > 60000) s = s.substr(0, 60000) + "\n...(truncated)";
    if (!s.empty() && s[0] == '\0') return "error: binary file";
    if (s.empty()) return "(empty file)";
    if (limit <= 0) limit = 2000;
    if (offset < 1) offset = 1;
    std::istringstream in(s);
    std::string line, out;
    int ln = 0, shown = 0;
    while (std::getline(in, line)) {
        ln++;
        if (ln < offset) continue;
        if (shown >= limit) break;
        if (line.size() > 2000) line = line.substr(0, 2000) + "…";
        out += std::to_string(ln) + ": " + line + "\n";
        shown++;
    }
    return out.empty() ? "(offset past the end)" : out;
}

std::string writeFile(const std::string& path, const std::string& content) {
    if (content.size() > 2000000) return "error: content too large";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return "error: cannot write " + path;
    f << content;
    return "wrote " + std::to_string(content.size()) + " bytes to " + path;
}

std::string editFile(const std::string& path, const std::string& oldText,
                     const std::string& newText, bool replaceAll) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "error: cannot open " + path;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (oldText.empty()) return "error: old text must not be empty";
    if (oldText == newText) return "error: old and new are identical";
    size_t pos = s.find(oldText);
    if (pos == std::string::npos) return "error: old text not found";
    if (!replaceAll && s.find(oldText, pos + 1) != std::string::npos) {
        return "error: old text appears more than once (use replaceAll "
               "or more context)";
    }
    size_t n = 0;
    while ((pos = s.find(oldText, pos)) != std::string::npos) {
        s.replace(pos, oldText.size(), newText);
        pos += newText.size();
        n++;
        if (!replaceAll) break;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return "error: cannot write " + path;
    out << s;
    return "edited " + path + " (" + std::to_string(n) + " occurrence(s))";
}

// The codex-style multi-file patch (*** Begin Patch ... *** End Patch).
// For the Update hunks the OLD lines come first, the NEW lines follow the
// "*** Begin Replacement" marker.
std::string applyPatch(const std::string& patchText) {
    std::istringstream in(patchText);
    std::string line;
    std::string mode;      // Add | Update | Delete | ""
    std::string path;
    std::string oldText, newText, cur;
    std::string report;
    bool collectingOld = true;  // Update mode: old vs new block
    auto flush = [&]() -> std::string {
        if (mode == "Add") return writeFile(path, cur);
        if (mode == "Delete") {
            if (std::remove(path.c_str()) != 0) {
                return "error: cannot delete " + path;
            }
            return "deleted " + path;
        }
        if (mode == "Update") {
            if (!oldText.empty() && !oldText.back()) oldText.pop_back();
            return editFile(path, oldText, newText, false);
        }
        return "";
    };
    bool began = false;
    while (std::getline(in, line)) {
        if (line == "*** Begin Patch") { began = true; continue; }
        if (!began) continue;
        if (line == "*** End Patch") break;
        if (line.rfind("*** Add File:", 0) == 0) {
            if (!path.empty()) report += flush() + "\n";
            mode = "Add"; path = line.substr(13);
            while (!path.empty() && path.front() == ' ') path.erase(0, 1);
            cur.clear();
        } else if (line.rfind("*** Update File:", 0) == 0) {
            if (!path.empty()) report += flush() + "\n";
            mode = "Update"; path = line.substr(16);
            while (!path.empty() && path.front() == ' ') path.erase(0, 1);
            oldText.clear(); newText.clear(); collectingOld = true;
        } else if (line.rfind("*** Delete File:", 0) == 0) {
            if (!path.empty()) report += flush() + "\n";
            mode = "Delete"; path = line.substr(16);
            while (!path.empty() && path.front() == ' ') path.erase(0, 1);
            cur.clear();
        } else if (line == "*** End of File") {
            if (!path.empty()) { report += flush() + "\n"; path.clear(); }
        } else if (line == "*** Begin Change" || line == "*** End Change" ||
                   line == "*** End Replacement") {
            continue;
        } else if (line == "*** Begin Replacement") {
            collectingOld = false;
            if (!newText.empty() && !newText.empty()) {}
        } else if (mode == "Update") {
            if (collectingOld) oldText += line + "\n";
            else newText += line + "\n";
        } else {
            cur += line + "\n";
        }
    }
    if (!path.empty()) report += flush() + "\n";
    return report.empty() ? "(patch applied nothing)" : report;
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

// The shell with an optional timeout: the `timeout` utility wraps the
// command (popen alone cannot kill a stuck child).
std::string shellCmd(const std::string& cmd, int timeoutSec,
                     const std::string& workdir) {
    if (timeoutSec <= 0) timeoutSec = 60;
    if (timeoutSec > 600) timeoutSec = 600;
    std::string inner = cmd;
    if (!workdir.empty()) inner = "cd " + json(workdir).dump() + " && " + inner;
    std::string wrapped = "timeout " + std::to_string(timeoutSec) + " sh -c "
                        + json(inner).dump() + " 2>&1";
    FILE* f = popen(wrapped.c_str(), "r");
    if (!f) return "error: popen failed";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) out += buf;
    int rc = pclose(f);
    return truncateOut(out) + (rc != 0 ? "\n[exit " + std::to_string(rc) + "]" : "");
}

std::string webFetch(const std::string& url) {
    http::Client c;
    c.setTimeout(20);
    auto r = c.get(url);
    if (!r.ok()) return "error: HTTP " + std::to_string(r.status_code);
    if (r.body.size() > 30000) r.body = r.body.substr(0, 30000) + "\n...(truncated)";
    return r.body;
}

std::string todoTool(const std::string& argsJson) {
    try {
        json args = json::parse(argsJson);
        g_todos.clear();
        for (const auto& t : args.value("todos", json::array())) {
            g_todos.push_back({t.value("status", "pending"),
                               t.value("content", "")});
        }
        std::string out;
        for (const auto& [st, content] : g_todos) {
            out += (st == "in_progress" ? "→ " : st == "completed" ? "✓ "
                                                                    : "· ")
                 + content + "\n";
        }
        return out.empty() ? "(todo list cleared)" : out;
    } catch (...) {
        return "error: bad todos JSON";
    }
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
                        const std::function<bool(const std::string&)>& confirm,
                        const std::function<std::string(const std::string&)>& ask) {
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
        Verdict v = checkTrust(cfg, cmd);
        if (v == Verdict::Deny) return "denied by the trust policy: " + cmd;
        if (v == Verdict::Ask && confirm) {
            if (!confirm(cmd)) return "declined by the user: " + cmd;
        }
        return shellCmd(cmd, i64("timeout", 60), str("workdir"));
    }
    if (name == "read_file") {
        return readFile(str("path"), i64("offset", 1), i64("limit", 2000));
    }
    if (name == "write_file") {
        if (cfg.planMode && str("path") != cfg.planFile) {
            return "denied: plan mode — only the plan file may be written";
        }
        return writeFile(str("path"), str("content"));
    }
    if (name == "edit_file") {
        if (cfg.planMode && str("path") != cfg.planFile) {
            return "denied: plan mode — only the plan file may be edited";
        }
        return editFile(str("path"), str("old"), str("new"),
                        args.value("replaceAll", false));
    }
    if (name == "apply_patch") {
        return cfg.planMode ? "denied: plan mode — no patches yet"
                            : applyPatch(str("patch_text"));
    }
    if (name == "glob") return globFiles(str("pattern"));
    if (name == "grep") return grepFiles(str("pattern"), str("path"));
    if (name == "webfetch") return webFetch(str("url"));
    if (name == "todo") return todoTool(argsJson);
    if (name == "question") {
        if (!ask) return "error: the question tool is not available";
        return ask(argsJson);
    }
    if (name == "clock") {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
        return buf;
    }
    return "unknown tool: " + name;
}

Result run(const Config& cfg, const std::string& prompt,
           std::vector<Message>& history,
           const std::function<bool(const std::string&)>& confirm,
           const std::function<std::string(const std::string&)>& ask,
           const std::function<void(const std::string&)>& log) {
    Result result;
    if (!cfg.cwd.empty() && chdir(cfg.cwd.c_str()) != 0 && log) {
        log("cannot chdir to " + cfg.cwd);
    }
    const std::string systemPrompt =
        "You are a CLI coding agent running inside the progressive-cli "
        "Matrix client. You help with the local filesystem and the shell. "
        "Use the provided tools for anything that needs the computer. "
        "Prefer small, verifiable steps; keep the todo tool updated for "
        "multi-step work. Answer concisely in the user's language; only "
        "use the tools when they are actually needed. The shell commands "
        "are subject to the user's trust policy.";

    history.push_back({"user", prompt, {}, "", ""});

    http::Client httpClient;
    httpClient.setTimeout(120);

    // Doom-loop protection: 3 identical consecutive tool calls in a row.
    std::string lastTool;
    int repeat = 0;

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
        history.push_back(assistant);
        for (const auto& c : assistant.calls) {
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
            std::string out = executeTool(cfg, c.name, c.args, confirm, ask);
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

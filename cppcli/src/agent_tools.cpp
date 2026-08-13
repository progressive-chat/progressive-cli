#include "agent_tools.hpp"

#include "../lib/http/http.hpp"
#include "../lib/json/json.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fnmatch.h>
#include <fstream>
#include <glob.h>
#include <map>
#include <memory>
#include <unordered_set>
#include <regex>
#include <sstream>
#include <sys/stat.h>
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
           "Fetch a URL and return its text content, capped at maxChars "
           "(default 30000).",
           {{"url", {{"type", "string"}}},
            {"maxChars", {{"type", "integer"},
                          {"description", "the max characters to return"}}}},
           {"url"});
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
    schema("memory",
           "Manage the agent's memory files (persist across sessions in "
           ".agent-memory/). Actions: list, read, create, edit (an exact "
           "old-string replace that fails on duplicates), rename, delete.",
           {{"action", {{"type", "string"},
                        {"enum", {"list", "read", "create", "edit", "rename",
                                  "delete"}}}},
            {"name", {{"type", "string"}}},
            {"content", {{"type", "string"}}},
            {"old_string", {{"type", "string"}}},
            {"new_string", {{"type", "string"}}},
            {"new_name", {{"type", "string"}}}},
           {"action"});
    schema("search_sessions",
           "Search the saved agent sessions for a keyword and return the "
           "matching excerpts (the RAG over the conversation history).",
           {{"query", {{"type", "string"}}}}, {"query"});
    {
        json props = json::object();
        props["target"] = {{"type", "string"}, {"enum", {"memory", "user"}}};
        props["action"] = {{"type", "string"},
                           {"enum", {"add", "replace", "remove"}}};
        props["content"] = {{"type", "string"}};
        props["old_text"] = {{"type", "string"}};
        json opProps = json::object();
        opProps["action"] = {{"type", "string"},
                             {"enum", {"add", "replace", "remove"}}};
        opProps["content"] = {{"type", "string"}};
        opProps["old_text"] = {{"type", "string"}};
        props["operations"] = {{"type", "array"},
                               {"items", {{"type", "object"},
                                          {"properties", opProps}}}};
        out.push_back({{"name", "notes"},
                       {"description",
                        "The persistent notes (the hermes-style memory). "
                        "Two targets: 'memory' (your own notes, 2200 chars) "
                        "and 'user' (the user profile, 1375 chars). Actions: "
                        "add / replace (the old_text substring) / remove, "
                        "or an atomic operations[] batch "
                        "[{action, old_text, content}] validated against "
                        "the final budget."},
                       {"input_schema",
                        {{"type", "object"}, {"properties", props},
                         {"required", {"target"}}}}});
    }
    schema("task",
           "Launch a subagent to work independently and return its final "
           "answer. Use for searches and non-mutating research "
           "(subagent_type: explore) or for general delegation (general).",
           {{"description", {{"type", "string"},
                             {"description", "3-5 word description of the task"}}},
            {"prompt", {{"type", "string"}}},
            {"subagent_type", {{"type", "string"},
                               {"enum", {"explore", "general"}}}}},
           {"description", "prompt"});
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
    bool collectingOld = true;
    auto flush = [&]() -> std::string {
        if (mode == "Add") return writeFile(path, cur);
        if (mode == "Delete") {
            if (std::remove(path.c_str()) != 0) {
                return "error: cannot delete " + path;
            }
            return "deleted " + path;
        }
        if (mode == "Update") {
            if (!oldText.empty() && oldText.back() == '\n') oldText.pop_back();
            if (!newText.empty() && newText.back() == '\n') newText.pop_back();
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

// The shell: an optional timeout via the `timeout` utility, an optional
// bubblewrap sandbox (the filesystem read-only except the cwd).
std::string shellCmd(const std::string& cmd, int timeoutSec,
                     const std::string& workdir, const std::string& sandbox) {
    if (timeoutSec <= 0) timeoutSec = 60;
    if (timeoutSec > 600) timeoutSec = 600;
    std::string inner = cmd;
    if (!workdir.empty()) inner = "cd " + json(workdir).dump() + " && " + inner;
    if (sandbox == "bwrap") {
        // bubblewrap: the root read-only, the cwd read-write, a private
        // tmp, the network blocked by default.
        std::string root;
        if (!workdir.empty()) root = workdir;
        else {
            char cwd[4096];
            if (getcwd(cwd, sizeof(cwd))) root = cwd;
        }
        inner = "bwrap --ro-bind / / --bind " + json(root).dump() + " "
              + json(root).dump()
              + " --tmpfs /tmp --unshare-net --die-with-parent -- "
              + inner;
    }
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

std::string webFetch(const std::string& url, int maxChars) {
    http::Client c;
    c.setTimeout(20);
    auto r = c.get(url);
    if (!r.ok()) return "error: HTTP " + std::to_string(r.status_code);
    if (maxChars <= 0) maxChars = 30000;
    if (maxChars > 500000) maxChars = 500000;
    if (static_cast<int>(r.body.size()) > maxChars) {
        r.body = r.body.substr(0, static_cast<size_t>(maxChars))
               + "\n...(truncated)";
    }
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

// ---- the memory files + the session search (the agora tool catalog) ----

std::string memoryTool(const std::string& argsJson) {
    json args = json::object();
    try {
        args = json::parse(argsJson);
    } catch (...) {
        return "error: bad arguments JSON";
    }
    std::string action = args.value("action", "");
    std::string name = args.value("name", "");
    const std::string dir = ".agent-memory";
    mkdir(dir.c_str(), 0755);
    auto pathFor = [&](const std::string& n) { return dir + "/" + n + ".md"; };
    if (action == "list") {
        glob_t g{};
        std::string out;
        if (glob((dir + "/*.md").c_str(), 0, nullptr, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; ++i) {
                std::string p = g.gl_pathv[i];
                std::string n = p.substr(dir.size() + 1);
                n = n.substr(0, n.size() - 3);  // drop .md
                out += n + "\n";
            }
            globfree(&g);
        }
        return out.empty() ? "(no memory files)" : out;
    }
    if (name.empty()) return "error: the memory tool needs a name";
    if (action == "read") return readFile(pathFor(name), 1, 2000);
    if (action == "create") {
        return writeFile(pathFor(name), args.value("content", ""));
    }
    if (action == "edit") {
        return editFile(pathFor(name), args.value("old_string", ""),
                        args.value("new_string", ""), false);
    }
    if (action == "rename") {
        std::string nn = args.value("new_name", "");
        if (nn.empty()) return "error: new_name required";
        if (std::rename(pathFor(name).c_str(), pathFor(nn).c_str()) != 0) {
            return "error: cannot rename " + name;
        }
        return "renamed " + name + " -> " + nn;
    }
    if (action == "delete") {
        if (std::remove(pathFor(name).c_str()) != 0) {
            return "error: cannot delete " + name;
        }
        return "deleted " + name;
    }
    return "error: unknown memory action " + action;
}

std::string searchSessions(const std::string& query) {
    if (query.empty()) return "error: empty query";
    glob_t g{};
    std::string out;
    int hits = 0;
    if (glob(".agent-sessions/*.json", 0, nullptr, &g) != 0) {
        globfree(&g);
        return "(no saved sessions)";
    }
    for (size_t i = 0; i < g.gl_pathc && hits < 20; ++i) {
        std::ifstream f(g.gl_pathv[i]);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string body = ss.str();
        if (body.find(query) == std::string::npos) continue;
        // Find the excerpt around the first hit.
        size_t pos = body.find(query);
        size_t a = pos > 120 ? pos - 120 : 0;
        size_t b = std::min(body.size(), pos + query.size() + 160);
        std::string excerpt = body.substr(a, b - a);
        // Compact the JSON a bit.
        for (char& ch : excerpt) {
            if (ch == '\n') ch = ' ';
        }
        out += std::string(g.gl_pathv[i]) + ": ..." + excerpt + "...\n";
        hits++;
    }
    globfree(&g);
    return out.empty() ? "(no matches in the sessions)" : out;
}

// ---- the permission engine ----

enum class Verdict { Allow, Ask, Deny };

// The per-tool glob rules: the LAST matching rule wins (opencode-style).
Verdict checkPermission(const Config& cfg, const std::string& tool,
                        const std::string& subject) {
    Verdict v = Verdict::Allow;  // the default: file tools are allowed
    for (const auto& r : cfg.rules) {
        if (r.tool != "*" && r.tool != tool) continue;
        if (fnmatch(r.glob.c_str(), subject.c_str(), 0) != 0) continue;
        v = r.action == "deny"   ? Verdict::Deny
          : r.action == "ask"    ? Verdict::Ask
                                 : Verdict::Allow;
    }
    return v;
}

// The trust policy for the shell: denyPrefixes > allowPrefixes > the
// permission rules > the level.
Verdict checkTrust(const Config& cfg, const std::string& cmd) {
    for (const auto& p : cfg.denyPrefixes) {
        if (cmd.rfind(p, 0) == 0) return Verdict::Deny;
    }
    for (const auto& p : cfg.allowPrefixes) {
        if (cmd.rfind(p, 0) == 0) return Verdict::Allow;
    }
    Verdict rule = checkPermission(cfg, "shell", cmd);
    if (rule != Verdict::Allow) return rule;
    if (cfg.trust == "allow") return Verdict::Allow;
    if (cfg.trust == "deny") return Verdict::Deny;
    return Verdict::Ask;
}

// ---- the MCP client (stdio JSON-RPC) ----

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

// ---- the notes (the hermes MEMORY.md / USER.md) ----

static std::vector<std::string> splitEntries(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        auto next = s.find("\n\u00a7\n", pos);
        std::string e = next == std::string::npos ? s.substr(pos)
                                                  : s.substr(pos, next - pos);
        if (!e.empty() || next != std::string::npos) out.push_back(e);
        if (next == std::string::npos) break;
        pos = next + 3;
    }
    return out;
}

static std::string notesPath(const std::string& target) {
    return std::string(".agent-memory/") + (target == "user" ? "USER.md"
                                                             : "MEMORY.md");
}

static std::string applyNotesOp(const std::string& body,
                                const std::string& action,
                                const std::string& content,
                                const std::string& oldText,
                                bool* okOut) {
    *okOut = false;
    auto entries = splitEntries(body);
    auto join = [&]() {
        std::string out;
        for (const auto& e : entries) out += e + "\n\u00a7\n";
        return out;
    };
    if (action == "add") {
        if (content.empty()) return "error: empty note";
        for (const auto& e : entries) {
            if (e == content) { *okOut = true; return body; }  // idempotent
        }
        entries.push_back(content);
        *okOut = true;
        return join();
    }
    if (action == "replace" || action == "remove") {
        bool found = false;
        for (auto& e : entries) {
            auto p = e.find(oldText);
            if (p != std::string::npos) {
                if (e.find(oldText, p + 1) != std::string::npos) {
                    return "error: old_text matches multiple times in one note";
                }
                e = action == "replace" ? content : "";
                found = true;
            }
        }
        if (!found) return "error: old_text not found";
        if (action == "remove") {
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [](const std::string& e) {
                                             return e.empty();
                                         }),
                          entries.end());
        }
        *okOut = true;
        return join();
    }
    return "error: unknown action " + action;
}

static std::string notesTool(const std::string& argsJson) {
    json args = json::object();
    try {
        args = json::parse(argsJson);
    } catch (...) {
        return "error: bad arguments JSON";
    }
    std::string target = args.value("target", "memory");
    if (target != "memory" && target != "user") return "error: bad target";
    int budget = target == "user" ? 1375 : 2200;
    std::string path = notesPath(target);
    mkdir(".agent-memory", 0755);
    std::string body;
    {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            body = ss.str();
        }
    }
    struct Op {
        std::string action;
        std::string content;
        std::string oldText;
    };
    std::vector<Op> ops;
    if (args.contains("operations") && args["operations"].is_array()) {
        for (const auto& o : args["operations"]) {
            ops.push_back({o.value("action", ""), o.value("content", ""),
                           o.value("old_text", "")});
        }
    } else {
        ops.push_back({args.value("action", ""), args.value("content", ""),
                       args.value("old_text", "")});
    }
    std::string working = body;
    for (const auto& op : ops) {
        bool ok = false;
        std::string res = applyNotesOp(working, op.action, op.content,
                                       op.oldText, &ok);
        if (!ok) {
            return res + "\n(no changes applied)";
        }
        // The final budget check on the whole batch result.
        working = res;
        if (static_cast<int>(working.size()) > budget) {
            return "error: over the " + std::to_string(budget)
                 + "-char budget — consolidate or remove notes first";
        }
    }
    // The atomic write.
    {
        std::string tmp = path + ".tmp";
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << working;
        f.close();
        std::rename(tmp.c_str(), path.c_str());
    }
    int used = static_cast<int>(working.size());
    std::string out = target + " notes updated (" + std::to_string(used) + "/"
                    + std::to_string(budget) + " chars)";
    return out;
}

// ---- the dangerous-command hardline (hermes DANGEROUS_PATTERNS) ----

bool isDangerousCommand(const std::string& cmd) {
    static const std::vector<std::regex> pats = {
        std::regex(R"(rm\s+-\S*[rf]\S*\s+/)", std::regex::icase),
        std::regex(R"(rm\s+-\S*[rf]\S*\s+(~|\$HOME|/etc|/usr|/bin))", std::regex::icase),
        std::regex(R"(sudo\s+rm)", std::regex::icase),
        std::regex(R"(\bmkfs\b)", std::regex::icase),
        std::regex(R"(dd\s+if=.*\s+of=/dev/[a-z]+)", std::regex::icase),
        std::regex(R"(\b(shutdown|poweroff|halt|reboot)\b)", std::regex::icase),
        std::regex(R"(\(\s*\)\s*[:;|]\s*\(\s*\)\s*[:;|]\s*\(\s*\)\s*[:;|]\s*(sh|bash|zsh))", std::regex::icase),
        std::regex(R"(:\s*\(\s*\)\s*\{)"),
        std::regex(R"(>\s*/dev/[a-z]+)"),
        std::regex(R"(\bchmod\s+-R\s+777)"),
    };
    for (const auto& p : pats) {
        if (std::regex_search(cmd, p)) return true;
    }
    return false;
}

// ---- the e-stop ----

bool eStopEngaged(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) return true;  // exists = engaged
    return false;
}

// ---- the schedule parser (the hermes parse_schedule subset) ----

static int64_t parseDurationSpec(const std::string& s) {
    std::smatch m;
    std::regex re(R"(^\s*(\d+)\s*(m|min|mins|minute|minutes|h|hr|hrs|hour|hours|d|day|days)\s*$)");
    if (!std::regex_match(s, m, re)) return -1;
    int64_t n = std::stoll(m[1].str());
    std::string unit = m[2].str();
    if (unit[0] == 'm') return n * 60;
    if (unit[0] == 'h') return n * 3600;
    return n * 86400;
}

static bool cronFieldMatch(int val, const std::string& field, int lo, int hi) {
    for (const std::string& part : [&]() -> std::vector<std::string> {
             std::vector<std::string> out;
             std::stringstream ss(field);
             std::string p;
             while (std::getline(ss, p, ',')) out.push_back(p);
             return out;
         }()) {
        if (part == "*") return true;
        int a = -1, b = -1, step = 1;
        auto slash = part.find('/');
        std::string range = part;
        if (slash != std::string::npos) {
            step = std::atoi(part.substr(slash + 1).c_str());
            range = part.substr(0, slash);
        }
        auto dash = range.find('-');
        if (dash != std::string::npos) {
            a = std::atoi(range.substr(0, dash).c_str());
            b = std::atoi(range.substr(dash + 1).c_str());
        } else if (range != "*") {
            a = b = std::atoi(range.c_str());
        } else {
            a = lo; b = hi;
        }
        if (step <= 0) step = 1;
        if (a >= 0 && b >= 0) {
            for (int v = a; v <= b; v += step) {
                if (v == val) return true;
            }
        }
    }
    return false;
}

static int64_t nextCronRun(const std::vector<std::string>& fields, int64_t now) {
    std::tm tm{};
    std::time_t t = static_cast<std::time_t>(now);
    localtime_r(&t, &tm);
    tm.tm_sec = 0;
    tm.tm_min++;
    // Walk minute by minute (up to ~2 years) until every field matches.
    for (int64_t steps = 0; steps < 366 * 24 * 60 * 2; ++steps) {
        std::time_t cand = mktime(&tm);
        std::tm c{};
        localtime_r(&cand, &c);
        bool ok = cronFieldMatch(c.tm_min, fields[0], 0, 59) &&
                  cronFieldMatch(c.tm_hour, fields[1], 0, 23) &&
                  cronFieldMatch(c.tm_mday, fields[2], 1, 31) &&
                  cronFieldMatch(c.tm_mon + 1, fields[3], 1, 12) &&
                  cronFieldMatch(c.tm_wday, fields[4], 0, 7);
        if (ok) return static_cast<int64_t>(cand);
        tm.tm_min++;
        cand = mktime(&tm);
        localtime_r(&cand, &tm);
    }
    return -1;
}

int64_t nextRunFromSpec(const std::string& spec, int64_t now) {
    std::string s = spec;
    // trim
    auto b = s.find_first_not_of(" \t");
    auto e = s.find_last_not_of(" \t");
    if (b == std::string::npos) return -1;
    s = s.substr(b, e - b + 1);
    if (s.rfind("every ", 0) == 0) {
        int64_t d = parseDurationSpec(s.substr(6));
        return d > 0 ? now + d : -1;
    }
    // the 5-field cron
    {
        std::stringstream ss(s);
        std::vector<std::string> fields;
        std::string p;
        while (ss >> p) fields.push_back(p);
        if (fields.size() >= 5) {
            bool looksCron = true;
            for (size_t i = 0; i < 5 && looksCron; ++i) {
                if (!std::regex_match(fields[i],
                        std::regex(R"(^[\d\*\-,/]+$)"))) looksCron = false;
            }
            if (looksCron) return nextCronRun(fields, now);
        }
    }
    int64_t d = parseDurationSpec(s);
    if (d > 0) return now + d;  // "30m" = one-shot in 30m
    // the ISO "YYYY-MM-DD[THH:MM:SS]"
    {
        std::tm tm{};
        if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm) ||
            strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm) ||
            strptime(s.c_str(), "%Y-%m-%d", &tm)) {
            std::time_t rt = mktime(&tm);
            if (rt > 0) return static_cast<int64_t>(rt);
        }
    }
    return -1;
}

// ---- the cron jobs store ----

void saveCronJobs(const std::string& path, const std::vector<CronJob>& jobs) {
    json arr = json::array();
    for (const auto& j : jobs) {
        arr.push_back({{"id", j.id}, {"spec", j.spec}, {"prompt", j.prompt},
                       {"next_run", j.nextRun},
                       {"monitor_url", j.monitorUrl},
                       {"monitor_hash", j.monitorHash}});
    }
    std::ofstream f(path, std::ios::trunc);
    f << arr.dump(2);
}

bool loadCronJobs(const std::string& path, std::vector<CronJob>& jobs) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    json arr;
    try {
        arr = json::parse(ss.str());
    } catch (...) {
        return false;
    }
    jobs.clear();
    for (const auto& j : arr) {
        CronJob cj;
        cj.id = j.value("id", "");
        cj.spec = j.value("spec", "");
        cj.prompt = j.value("prompt", "");
        cj.nextRun = j.value("next_run", 0);
        cj.monitorUrl = j.value("monitor_url", "");
        cj.monitorHash = j.value("monitor_hash", "");
        jobs.push_back(cj);
    }
    return true;
}

// ---- session persistence ----

void saveSession(const std::string& path, const std::vector<Message>& history) {
    json arr = json::array();
    for (const auto& m : history) {
        json j = {{"role", m.role}, {"content", m.content}};
        if (!m.calls.empty()) {
            json tcs = json::array();
            for (const auto& c : m.calls) {
                tcs.push_back({{"id", c.id}, {"name", c.name}, {"args", c.args}});
            }
            j["calls"] = tcs;
        }
        if (!m.toolCallId.empty()) j["toolCallId"] = m.toolCallId;
        if (!m.toolName.empty()) j["toolName"] = m.toolName;
        arr.push_back(j);
    }
    std::ofstream f(path, std::ios::trunc);
    f << arr.dump(2);
}

bool loadSession(const std::string& path, std::vector<Message>& history) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    json arr;
    try {
        arr = json::parse(ss.str());
    } catch (...) {
        return false;
    }
    history.clear();
    for (const auto& j : arr) {
        Message m;
        m.role = j.value("role", "");
        m.content = j.value("content", "");
        for (const auto& c : j.value("calls", json::array())) {
            ToolCall tc;
            tc.id = c.value("id", "");
            tc.name = c.value("name", "");
            tc.args = c.value("args", "");
            m.calls.push_back(tc);
        }
        m.toolCallId = j.value("toolCallId", "");
        m.toolName = j.value("toolName", "");
        history.push_back(m);
    }
    return true;
}

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
        auto choices = j.value("choices", json::array());
        if (choices.empty()) continue;
        auto delta = choices[0].value("delta", json::object());
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
           const std::function<int(const std::string&)>& confirm,
           const std::function<std::string(const std::string&)>& ask,
           const std::function<void(const std::string&)>& log,
           const std::function<void(const std::string&)>& onToken) {
    Result result;
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
        json reqBody;
        http::Request req;
        if (cfg.provider == "anthropic") {
            json messages = json::array();
            buildAnthropicMessages(history, systemPrompt, messages);
            reqBody = {{"model", cfg.model},
                       {"system", systemPrompt},
                       {"messages", messages},
                       {"tools", allSchemas},
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
                       {"tools", openAiToolsParam(allSchemas)}};
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
                });
            if (sresp.ok() && (acc.done || !acc.msg.content.empty() ||
                               !acc.msg.calls.empty())) {
                assistant = acc.msg;
                haveAssistant = true;
                result.streamed = true;
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
        }
        if (assistant.calls.empty()) {
            history.push_back(assistant);
            result.ok = true;
            result.text = assistant.content;
            if (result.streamed && onToken) onToken("\n");
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
            std::string out = executeTool(cfg, c.name, c.args,
                                          verdictConfirm, ask, log, 0);
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

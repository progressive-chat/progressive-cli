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
    schema("process",
           "An interactive background process in a pseudo-terminal, for "
           "debugging (gdb/pdb, REPLs, servers). start spawns the command "
           "and returns an id; send writes an input line; poll reads the "
           "new output; wait waits for the exit (timeout seconds); kill "
           "terminates; list shows the live processes. The processes die "
           "at the end of the turn.",
           {{"action", {{"type", "string"},
                        {"enum", {"start", "send", "poll", "wait", "kill", "list"}}}},
            {"id", {{"type", "string"},
                    {"description", "the process id from start"}}},
            {"command", {{"type", "string"},
                         {"description", "start: the command to run"}}},
            {"input", {{"type", "string"},
                       {"description", "send: the line written to the process"}}},
            {"timeout", {{"type", "integer"},
                         {"description", "wait: the seconds to wait (default 10)"}}}},
           {"action"});
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
           "task should be in_progress at a time. The items are objects "
           "with the content (string) and status (pending|in_progress|"
           "completed|cancelled) keys.",
           {{"todos", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
           {"todos"});
    schema("question",
           "Ask the user questions and return their answers as the tool "
           "result (the loop continues afterwards). The items are objects "
           "with the question, header, options (array of {label, "
           "description}) and multiple (boolean) keys.",
           {{"questions", {{"type", "array"},
                           {"items", {{"type", "object"}}}}}},
           {"questions"});
    schema("clock",
           "The current date and time (UTC).",
           json::object(), json::array());
    schema("skill",
           "Load a skill file (.agent-skills/<name>.md) into the context "
           "and return its content.",
           {{"name", {{"type", "string"}}}}, {"name"});
    schema("lsp",
           "Query the language server (clangd): hover (the type/signature "
           "at a position) or definition (where a symbol is declared).",
           {{"operation", {{"type", "string"},
                           {"enum", {"hover", "definition"}}}},
            {"path", {{"type", "string"}}},
            {"line", {{"type", "integer"}}},
            {"character", {{"type", "integer"}}}},
           {"operation", "path", "line", "character"});
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

std::string truncateOut(std::string s, size_t bytes, size_t lines) {
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

}} // namespace matrixcli::agenttools

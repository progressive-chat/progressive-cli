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

namespace {
class TodoBoard {
public:
    static TodoBoard& instance() {
        static TodoBoard board;
        return board;
    }
    std::string set(const json& todos) {
        todos_.clear();
        for (const auto& t : todos.value("todos", json::array())) {
            todos_.push_back({t.value("status", "pending"),
                              t.value("content", "")});
        }
        return render();
    }
    std::string render() const {
        std::string out;
        for (const auto& [status, content] : todos_) {
            out += (status == "in_progress" ? "→ "
                    : status == "completed"   ? "✓ "
                                              : "· ")
                 + content + "\n";
        }
        return out.empty() ? "(todo list cleared)" : out;
    }
    std::vector<std::pair<std::string, std::string>> items() const {
        return todos_;
    }
private:
    std::vector<std::pair<std::string, std::string>> todos_;
}; // anonymous namespace
namespace {

// The RAII popen guard: pclose on the way out; `close()` returns the
// exit status exactly once.
class PopenFile {
public:
    explicit PopenFile(FILE* f) : f_(f) {}
    PopenFile(const PopenFile&) = delete;
    PopenFile& operator=(const PopenFile&) = delete;
    ~PopenFile() { if (f_) pclose(f_); }
    FILE* get() const { return f_; }
    int close() {
        const int rc = f_ ? pclose(f_) : -1;
        f_ = nullptr;
        return rc;
    }
private:
    FILE* f_ = nullptr;
};

} // namespace

} // namespace

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
    const std::string wrapped = "timeout " + std::to_string(timeoutSec)
                              + " sh -c " + json(inner).dump() + " 2>&1";
    PopenFile pf(popen(wrapped.c_str(), "r"));
    if (!pf.get()) return "error: popen failed";
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pf.get())) out += buf;
    const int rc = pf.close();
    return truncateOut(out)
         + (rc != 0 ? "\n[exit " + std::to_string(rc) + "]" : "");
}
// ---- the permission engine ----

class ProcessManager {
public:
    static ProcessManager& instance() {
        static ProcessManager pm;
        return pm;
    }

    std::string start(const Config& cfg, const std::string& cmd,
                      const std::function<int(const std::string&)>& confirm) {
        if (!matrixcli::g_interrupted.load()) return "interrupted (Ctrl+C)";
        if (isDangerousCommand(cmd))
            return "hardline block: dangerous command refused — " + cmd;
        const Verdict v = checkTrust(cfg, cmd);
        if (v == Verdict::Deny) return "denied by the trust policy: " + cmd;
        if (v == Verdict::Ask && confirm &&
            confirm(cmd) == static_cast<int>(ConfirmVerdict::Decline))
            return "declined by the user: " + cmd;

        int master = -1, slave = -1;
        if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0)
            return "error: openpty failed";
        const pid_t pid = fork();
        if (pid == 0) {
            setsid();
            ioctl(slave, TIOCSCTTY, 0);
            dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
            close(slave); close(master);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(slave);
        if (pid < 0) { close(master); return "error: fork failed"; }
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

        auto p = std::make_shared<Proc>(pid, master);
        const std::string id = "p" + std::to_string(++seq_);
        {
            std::lock_guard lk(mtx_);
            procs_[id] = p;
        }
        std::thread(&ProcessManager::reader, this, p).detach();
        usleep(250000);  // the first prompt
        json j = json::parse(snapshot(p));
        j["id"] = id;
        j["started"] = cmd;
        return j.dump();
    }

    std::string send(const std::string& id, const std::string& input) {
        const auto p = find(id);
        if (!p) return "error: no such process: " + id;
        {
            std::lock_guard lk(p->mtx);
            if (p->exited || p->masterFd < 0) return "error: the process exited";
        }
        const std::string line = input + "\n";
        if (write(p->masterFd, line.c_str(), line.size()) < 0)
            return "error: write failed";
        usleep(300000);  // the debugger's response
        return snapshot(p);
    }

    std::string poll(const std::string& id) {
        const auto p = find(id);
        return p ? snapshot(p) : "error: no such process: " + id;
    }

    std::string wait(const std::string& id, int timeout) {
        const auto p = find(id);
        if (!p) return "error: no such process: " + id;
        timeout = std::clamp(timeout <= 0 ? 10 : timeout, 1, 300);
        for (int t = 0; t < timeout * 10; t++) {
            {
                std::lock_guard lk(p->mtx);
                if (p->exited) break;
            }
            usleep(100000);
        }
        return snapshot(p);
    }

    std::string kill(const std::string& id) {
        const auto p = find(id);
        if (!p) return "error: no such process: " + id;
        {
            std::lock_guard lk(p->mtx);
            if (!p->exited && p->pid > 0) {
                ::kill(p->pid, SIGKILL);
                p->killed = true;
            }
        }
        return snapshot(p);
    }

    std::string list() {
        json out = json::array();
        std::lock_guard lk(mtx_);
        for (const auto& [id, p] : procs_) {
            out.push_back({{"id", id},
                           {"status", p->exited ? "exited" : "running"}});
        }
        return out.dump();
    }

    // The turn is over — no process may outlive it (the guard in run()).
    void cleanupAll() {
        std::lock_guard lk(mtx_);
        for (const auto& [id, p] : procs_) {
            if (!p->exited && p->pid > 0) ::kill(p->pid, SIGKILL);
            if (p->masterFd >= 0) close(p->masterFd);
            p->masterFd = -1;
        }
        procs_.clear();
    }

private:
    struct Proc {
        Proc(pid_t pid_, int fd) : pid(pid_), masterFd(fd) {}
        int pid = -1;
        int masterFd = -1;
        std::string buffer;      // the captured output (capped ring)
        size_t consumed = 0;     // the agent already read up to here
        bool exited = false;
        bool killed = false;
        int exitCode = -1;
        std::mutex mtx;
    };

    // The reader: the master fd → the buffer; EOF reaps the child.
    void reader(std::shared_ptr<Proc> p) {
        char buf[4096];
        while (true) {
            const ssize_t n = read(p->masterFd, buf, sizeof(buf));
            if (n > 0) {
                std::lock_guard lk(p->mtx);
                p->buffer.append(buf, static_cast<size_t>(n));
                if (p->buffer.size() > 65536)
                    p->buffer.erase(0, p->buffer.size() - 65536);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK
                                 || errno == EINTR)) {
                usleep(20000);
            } else {
                break;  // the child closed the pty
            }
        }
        int st = 0;
        waitpid(p->pid, &st, 0);
        std::lock_guard lk(p->mtx);
        p->exited = true;
        if (WIFEXITED(st)) p->exitCode = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) p->exitCode = 128 + WTERMSIG(st);
        close(p->masterFd);
        p->masterFd = -1;
    }

    // The NEW output since the last tool call + the status.
    static std::string snapshot(const std::shared_ptr<Proc>& p) {
        std::lock_guard lk(p->mtx);
        const std::string fresh = p->buffer.substr(p->consumed);
        p->consumed = p->buffer.size();
        json j = {{"output", fresh},
                  {"status", p->exited ? "exited" : (p->killed ? "killed" : "running")}};
        if (p->exited) j["exit"] = p->exitCode;
        return j.dump();
    }

    std::shared_ptr<Proc> find(const std::string& id) {
        std::lock_guard lk(mtx_);
        const auto it = procs_.find(id);
        return it == procs_.end() ? nullptr : it->second;
    }

    std::map<std::string, std::shared_ptr<Proc>> procs_;
    std::mutex mtx_;
    int seq_ = 0;
};

std::string processTool(const Config& cfg, const std::string& argsJson,
                               const std::function<int(const std::string&)>& confirm) {
    json args = json::object();
    try { args = json::parse(argsJson); }
    catch (...) { return "error: bad arguments JSON"; }
    const std::string action = args.value("action", "");
    auto& pm = ProcessManager::instance();

    if (action == "start") {
        const std::string cmd = args.value("command", "");
        if (cmd.empty()) return "error: command required";
        return pm.start(cfg, cmd, confirm);
    }
    if (action == "send")
        return pm.send(args.value("id", ""), args.value("input", ""));
    if (action == "poll")
        return pm.poll(args.value("id", ""));
    if (action == "wait")
        return pm.wait(args.value("id", ""), args.value("timeout", 10));
    if (action == "kill")
        return pm.kill(args.value("id", ""));
    if (action == "list")
        return pm.list();
    return "error: unknown action (start|send|poll|wait|kill|list)";
}

// The turn is over — no process may outlive it (the guard in run()).
void processCleanupAll() {
    ProcessManager::instance().cleanupAll();
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
        return TodoBoard::instance().set(json::parse(argsJson));
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

// The user's request history: the prompts the user sent the agent in the
// saved sessions (stored separately from the chat rooms in
// .agent-sessions/). Newest sessions first, each with its user requests
// (truncated) and a rough turn count.
std::string requestHistory(int limit) {
    if (limit <= 0) limit = 10;
    glob_t g{};
    if (glob(".agent-sessions/*.json", 0, nullptr, &g) != 0) {
        globfree(&g);
        return "(no saved sessions)";
    }
    std::vector<std::pair<time_t, std::string>> files;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        struct stat st{};
        if (stat(g.gl_pathv[i], &st) != 0) continue;
        files.push_back({st.st_mtime, g.gl_pathv[i]});
    }
    globfree(&g);
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    int shown = 0;
    for (const auto& [mtime, path] : files) {
        if (shown >= limit) break;
        std::ifstream f(path);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        json arr;
        try {
            arr = json::parse(ss.str());
        } catch (...) {
            continue;
        }
        if (!arr.is_array()) continue;
        std::string reqs;
        int turns = 0;
        for (const auto& m : arr) {
            std::string role = m.value("role", "");
            if (role != "user" && role != "assistant") continue;
            turns++;
            if (role != "user") continue;
            std::string content = m.value("content", "");
            if (content.empty()) continue;
            if (!reqs.empty()) reqs += "\n";
            if (content.size() > 160) content = content.substr(0, 160) + "...";
            for (char& ch : content)
                if (ch == '\n') ch = ' ';
            reqs += "  - " + content;
        }
        if (reqs.empty()) continue;
        char ts[32];
        std::tm tm{};
        localtime_r(&mtime, &tm);
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);
        std::string name = path;
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        out += name + " (" + ts + ", " + std::to_string(turns) + " turns):\n"
             + reqs + "\n";
        shown++;
    }
    return out.empty() ? "(no user requests in the sessions)" : out;
}

// ---- the MCP client (stdio JSON-RPC) ----
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

// ---- the interactive process tool (gdb/pdb/REPL debugging) ----

//
// The shell tool is one-shot; a debugger needs a LIVE session: a
// pseudo-terminal the agent can write commands into and read the
// responses from across the tool calls of one turn.

// The interactive-process registry: a pseudo-terminal per process, the
// reader threads, the output rings and the ids — one object, no globals.

std::vector<std::pair<std::string, std::string>> agentTodos() {
    return TodoBoard::instance().items();
}

// ---- the notes (the hermes MEMORY.md / USER.md) ----

}} // namespace matrixcli::agenttools

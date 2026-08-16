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

std::string notesTool(const std::string& argsJson) {
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

// ---- the single agent config file ----

std::string agentConfigPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = xdg && *xdg ? std::string(xdg)
                                   : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.config";
    return base + "/matrixcli/agent.json";
}

bool loadAgentConfig(Config& cfg) {
    std::ifstream f(agentConfigPath());
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    json j;
    try {
        j = json::parse(ss.str());
    } catch (...) {
        return false;
    }
    cfg.provider = j.value("provider", cfg.provider);
    cfg.endpoint = j.value("endpoint", cfg.endpoint);
    cfg.model = j.value("model", cfg.model);
    cfg.key = j.value("key", cfg.key);
    cfg.trust = j.value("trust", cfg.trust);
    cfg.proxy = j.value("proxy", cfg.proxy);
    cfg.sandbox = j.value("sandbox", cfg.sandbox);
    cfg.allowPrefixes.clear();
    for (const auto& v : j.value("allow", json::array())) {
        if (v.is_string()) cfg.allowPrefixes.push_back(v.get<std::string>());
    }
    cfg.denyPrefixes.clear();
    for (const auto& v : j.value("deny", json::array())) {
        if (v.is_string()) cfg.denyPrefixes.push_back(v.get<std::string>());
    }
    cfg.rules.clear();
    for (const auto& v : j.value("rules", json::array())) {
        cfg.rules.push_back({v.value("tool", ""), v.value("glob", ""),
                             v.value("action", "")});
    }
    cfg.llmStyle = j.value("llm_style", cfg.llmStyle);
    cfg.llmMarkdown = j.value("llm_markdown", cfg.llmMarkdown);
    cfg.mcpServers.clear();
    for (const auto& v : j.value("mcp", json::array())) {
        cfg.mcpServers.push_back({v.value("name", ""), v.value("command", "")});
    }
    return true;
}

void saveAgentConfig(const Config& cfg) {
    json j = {{"provider", cfg.provider},
              {"endpoint", cfg.endpoint},
              {"model", cfg.model},
              {"key", cfg.key},
              {"trust", cfg.trust},
              {"proxy", cfg.proxy},
              {"sandbox", cfg.sandbox},
              {"llm_style", cfg.llmStyle},
              {"llm_markdown", cfg.llmMarkdown},
              {"allow", cfg.allowPrefixes},
              {"deny", cfg.denyPrefixes},
              {"mcp", json::array()}};
    json rules = json::array();
    for (const auto& r : cfg.rules) {
        rules.push_back({{"tool", r.tool}, {"glob", r.glob},
                         {"action", r.action}});
    }
    j["rules"] = rules;
    json mcp = json::array();
    for (const auto& m : cfg.mcpServers) {
        mcp.push_back({{"name", m.name}, {"command", m.command}});
    }
    j["mcp"] = mcp;
    std::string path = agentConfigPath();
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        mkdir(path.substr(0, slash).c_str(), 0700);
    }
    std::ofstream f(path, std::ios::trunc);
    f << j.dump(2);
    f.close();
    ::chmod(path.c_str(), 0600);  // the API key lives here
}

// ---- the standing goal (the hermes /goal + /subgoal) ----

void saveGoal(const std::string& path, const GoalState& g) {
    json j = {{"goal", g.goal},
              {"contract", g.contract},
              {"subgoals", g.subgoals},
              {"gate", g.gateCommand},
              {"max_turns", g.maxTurns},
              {"paused", g.paused},
              {"achieved", g.achieved},
              {"turns_used", g.turnsUsed}};
    std::ofstream f(path, std::ios::trunc);
    f << j.dump(2);
}

bool loadGoal(const std::string& path, GoalState& g) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    json j;
    try {
        j = json::parse(ss.str());
    } catch (...) {
        return false;
    }
    g.goal = j.value("goal", "");
    g.contract = j.value("contract", "");
    g.subgoals.clear();
    for (const auto& s : j.value("subgoals", json::array())) {
        if (s.is_string()) g.subgoals.push_back(s.get<std::string>());
    }
    g.gateCommand = j.value("gate", "");
    g.maxTurns = j.value("max_turns", 50);
    g.paused = j.value("paused", false);
    g.achieved = j.value("achieved", false);
    g.turnsUsed = j.value("turns_used", 0);
    return true;
}

// The continuation block appended to the system prompt when a goal is set.
std::string draftContract(const Config& cfg, const std::string& objective) {
    if (cfg.key.empty()) return "";
    std::vector<Message> hist;
    hist.push_back({"user",
                    "Draft a completion contract for this objective:\n"
                    + objective +
                    "\n\nReply with exactly these sections and nothing else:\n"
                    "## Outcome\n<one sentence>\n## Verification\n<how to prove "
                    "it with concrete evidence>\n## Constraints\n<must-not>\n"
                    "## Boundaries\n<in scope / out of scope>\n## Stop when\n"
                    "<when to stop and ask the user>",
                    {}, "", ""});
    Result r = run(cfg, "draft the contract", hist, nullptr, nullptr, nullptr,
                   nullptr);
    return r.ok ? r.text : "";
}

// The goal judge: the gates first, then the LLM verdict.
std::string judgeGoal(const Config& cfg, const GoalState& goal,
                      const std::vector<Message>& history,
                      const std::function<void(const std::string&)>& log) {
    // 1. The deterministic quality gates.
    if (!goal.gateCommand.empty()) {
        std::string gateOut = shellCmd(goal.gateCommand, 300, "", cfg.sandbox);
        if (gateOut.find("[exit 0]") == std::string::npos &&
            gateOut.find("(no output)") == std::string::npos &&
            !gateOut.empty()) {
            // A failed gate (the non-zero exit): its output becomes the
            // feedback — the agent iterates against the evidence.
            return "gate failed — keep working:\n" + gateOut.substr(0, 3000);
        }
    }
    if (cfg.key.empty()) return "no API key — the judge needs the LLM";
    // 2. The LLM verdict.
    std::string recent;
    int shown = 0;
    for (auto it = history.rbegin(); it != history.rend() && shown < 4000;
         ++it) {
        if (!it->content.empty() && it->role != "tool") {
            std::string part = it->content.substr(0, 4000 - shown);
            recent = part + "\n" + recent;
            shown += static_cast<int>(part.size());
        }
    }
    std::vector<Message> judgeHist;
    std::string prompt =
        "You are the goal judge. The user's standing goal:\nGoal: "
        + goal.goal + "\n\n";
    if (!goal.contract.empty()) prompt += "Completion contract:\n"
                                          + goal.contract + "\n\n";
    if (!goal.subgoals.empty()) {
        prompt += "Additional criteria:\n";
        for (const auto& s : goal.subgoals) prompt += "- " + s + "\n";
        prompt += "\n";
    }
    prompt += "The agent's latest work:\n" + recent +
              "\n\nVerdict: is the goal (and every additional criterion) "
              "achieved with concrete evidence? Reply with exactly one JSON "
              "line: {\"done\": true|false, \"reason\": \"...\"}";
    judgeHist.push_back({"user", prompt, {}, "", ""});
    Result r = run(cfg, "judge the goal", judgeHist, nullptr, nullptr,
                   [&](const std::string& l) { if (log) log(l); }, nullptr);
    if (!r.ok) return "judge error: " + r.error;
    // The tolerant verdict parse.
    std::string text = r.text;
    auto b = text.find('{');
    auto e = text.rfind('}');
    if (b != std::string::npos && e != std::string::npos && e > b) {
        try {
            json v = json::parse(text.substr(b, e - b + 1));
            bool done = v.value("done", false);
            std::string reason = v.value("reason", "");
            if (done) {
                return "goal achieved: " + reason;
            }
            return "not done yet — " + reason;
        } catch (...) {}
    }
    return "judge: " + text.substr(0, 400);
}

}} // namespace matrixcli::agenttools

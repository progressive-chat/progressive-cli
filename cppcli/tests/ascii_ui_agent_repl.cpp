// src/ascii_ui.cpp — ASCII-drawn client interface for the CLI (not the TUI).
//
// `progressive-cli ui` draws a chat-client-like layout with plain characters:
// a header, a left panel with the room list, the open room's messages in
// the center and the member list on the right, panels separated by pipes.
// It is a REPL: every command executes and the whole frame is redrawn —
// no auto-updates, no raw terminal mode (works in any terminal, scrolls
// like a normal CLI program).
#include "ascii_ui.hpp"
#include "ascii_state.hpp"
#include "commands.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/string_utils.hpp"
#include "agent_tools.hpp"
#include <cstdlib>
#include <glob.h>
#include <poll.h>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "cli/args.hpp"
#include "pcore.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

#include "ascii_ui_impl.hpp"

namespace matrixcli {

int asciiAgentReplDispatch(UiState& st, db::Database& dbi, const cli::Args& a) {
        if (a.command == "agent") {
            static std::vector<agenttools::Message> agentHistory;
            static bool agentHistoryLoaded = false;
            static std::vector<std::string> agentPromptHistory;
            if (!agentHistoryLoaded) {
                // Resume the previous conversation (the auto-saved one).
                agenttools::loadSession(".agent-sessions/last.json",
                                        agentHistory);
                agentHistoryLoaded = true;
            }
            agenttools::Config cfg;
            cfg.provider = dbi.getSetting("agent_provider", "openai");
            cfg.endpoint = dbi.getSetting("agent_endpoint", "");
            cfg.model = dbi.getSetting("agent_model", "");
            cfg.key = dbi.getSetting("agent_key", "");
            cfg.trust = dbi.getSetting("agent_trust", "ask");
            if (cfg.key.empty()) {
                const char* env = cfg.provider == "anthropic"
                                      ? std::getenv("ANTHROPIC_API_KEY")
                                      : std::getenv("OPENAI_API_KEY");
                if (env && *env) cfg.key = env;
            }
            if (cfg.model.empty()) {
                cfg.model = cfg.provider == "anthropic"
                                ? "claude-3-5-haiku-20241022" : "gpt-4o-mini";
            }
            {
                auto loadCsv = [&](const std::string& key,
                                   std::vector<std::string>& out) {
                    std::string v = dbi.getSetting(key, "");
                    std::string cur;
                    for (char ch : v) {
                        if (ch == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
                        else cur += ch;
                    }
                    if (!cur.empty()) out.push_back(cur);
                };
                loadCsv("agent_allow", cfg.allowPrefixes);
                loadCsv("agent_deny", cfg.denyPrefixes);
                std::vector<std::string> rules;
                loadCsv("agent_rules", rules);
                for (const auto& r : rules) {
                    auto a = r.find('|');
                    auto b = r.rfind('|');
                    if (a != std::string::npos && b != std::string::npos && a < b) {
                        cfg.rules.push_back({r.substr(0, a), r.substr(a + 1, b - a - 1),
                                             r.substr(b + 1)});
                    }
                }
                std::vector<std::string> mcps;
                loadCsv("agent_mcp", mcps);
                for (const auto& m : mcps) {
                    auto p = m.find('|');
                    if (p != std::string::npos) {
                        cfg.mcpServers.push_back({m.substr(0, p), m.substr(p + 1)});
                    }
                }
                cfg.sandbox = dbi.getSetting("agent_sandbox", "off");
                cfg.proxy = dbi.getSetting("agent_proxy", "");
                agenttools::loadGoal(".agent-goal.json", cfg.goal);
                // The single config file is the source of truth.
                agenttools::loadAgentConfig(cfg);
            }
            char cwdbuf[4096];
            if (getcwd(cwdbuf, sizeof(cwdbuf))) cfg.cwd = cwdbuf;

            // Subcommands: trust / allow / deny / config / reset.
            if (!a.positional.empty() && a.positional[0] == "trust") {
                std::string v = a.positional.size() >= 2 ? a.positional[1] : "";
                if (v != "allow" && v != "ask" && v != "deny") {
                    std::cout << "Usage: agent trust <allow|ask|deny>" << std::endl;
                    return 1;
                }
                dbi.setSetting("agent_trust", v);
            cfg.trust = v;
            agenttools::saveAgentConfig(cfg);
                st.statusNote = "agent trust: " + v;
                return 1;
            }
            if (!a.positional.empty() &&
                (a.positional[0] == "allow" || a.positional[0] == "deny")) {
                if (a.positional.size() < 2) {
                    std::cout << "Usage: agent " << a.positional[0]
                              << " <command-prefix>" << std::endl;
                    return 1;
                }
                auto& list = a.positional[0] == "allow" ? cfg.allowPrefixes
                                                        : cfg.denyPrefixes;
                list.push_back(a.positional[1]);
                std::string csv;
                for (const auto& p : list) csv += (csv.empty() ? "" : ",") + p;
                dbi.setSetting(a.positional[0] == "allow" ? "agent_allow"
                                                          : "agent_deny", csv);
                agenttools::saveAgentConfig(cfg);
                st.statusNote = std::string("agent ") + a.positional[0] + ": "
                              + a.positional[1];
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "config") {
                if (a.positional.size() < 3) {
                    std::cout << "Usage: agent config provider|endpoint|model|key <value>"
                              << std::endl;
                    return 1;
                }
                std::string k = a.positional[1];
                std::string v = a.positional[2];
                if (k == "provider") {
                    if (agenttools::applyProviderPreset(cfg, v)) {
                        dbi.setSetting("agent_provider", cfg.provider);
                        dbi.setSetting("agent_endpoint", cfg.endpoint);
                        dbi.setSetting("agent_model", cfg.model);
                    } else {
                        dbi.setSetting("agent_provider", v);
                        cfg.provider = v;
                    }
                }
                else if (k == "endpoint") { dbi.setSetting("agent_endpoint", v); cfg.endpoint = v; }
                else if (k == "model") { dbi.setSetting("agent_model", v); cfg.model = v; }
                else if (k == "key") { dbi.setSetting("agent_key", v); cfg.key = v; }
                else {
                    std::cout << "agent config: provider|endpoint|model|key" << std::endl;
                    return 1;
                }
                st.statusNote = "agent " + k + " set";
                agenttools::saveAgentConfig(cfg);
                return 1;
            }
            auto confirm = [&](const std::string& cmd) -> int {
                std::cout << "  run: " << cmd << " [y/N/a/A] " << std::flush;
                std::string ans;
                std::getline(std::cin, ans);
                if (ans == "y" || ans == "Y") return 1;
                if (ans == "a") return 2;
                if (ans == "A") return 3;
                return 0;
            };
            auto ask = [&](const std::string& questionsJson) -> std::string {
                nlohmann::json qs;
                try { qs = nlohmann::json::parse(questionsJson); }
                catch (...) { return "error: bad questions JSON"; }
                std::string out;
                for (const auto& q : qs.value("questions", nlohmann::json::array())) {
                    std::cout << "  Q: " << q.value("question", "?") << std::endl;
                    auto opts = q.value("options", nlohmann::json::array());
                    for (size_t i = 0; i < opts.size(); ++i) {
                        std::cout << "    " << i + 1 << ") "
                                  << opts[i].value("label", "") << std::endl;
                    }
                    std::cout << "  answer> " << std::flush;
                    std::string ans;
                    std::getline(std::cin, ans);
                    out += "Q: " + q.value("question", "?") + "\nA: " + ans + "\n";
                }
                return out.empty() ? "(no questions answered)" : out;
            };
            auto log = [](const std::string& l) { std::cout << l << std::endl; };
            // The Esc watcher: a background thread polls the terminal for
            // the 0x1b byte while the agent runs — Esc stops the agent,
            // the program stays alive (the Ctrl+C exits as usual).
            auto startEscWatcher = []() -> std::shared_ptr<std::thread> {
                return std::make_shared<std::thread>([]() {
                    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
                    while (!matrixcli::g_agentInterrupt.load() &&
                           matrixcli::g_interrupted.load()) {
                        int pr = poll(&pfd, 1, 50);
                        if (pr <= 0) continue;
                        unsigned char c = 0;
                        if (read(STDIN_FILENO, &c, 1) == 1 && c == 0x1b) {
                            matrixcli::g_agentInterrupt = true;
                        }
                    }
                });
            };
            auto stopEscWatcher = [](std::shared_ptr<std::thread>& t) {
                if (t && t->joinable()) t->join();
                matrixcli::g_agentInterrupt = false;
            };
            if (!a.positional.empty() && a.positional[0] == "permit") {
                // agent permit <tool> <glob> <allow|ask|deny>
                if (a.positional.size() < 4) {
                    std::cout << "Usage: agent permit <read|write|edit|shell|"
                                 "apply_patch|*> <glob> <allow|ask|deny>" << std::endl;
                    return 1;
                }
                std::string act = a.positional[3];
                if (act != "allow" && act != "ask" && act != "deny") {
                    std::cout << "the action must be allow|ask|deny" << std::endl;
                    return 1;
                }
                cfg.rules.push_back({a.positional[1], a.positional[2], act});
                std::string csv;
                for (const auto& r : cfg.rules) {
                    csv += (csv.empty() ? "" : ",") + r.tool + "|" + r.glob
                         + "|" + r.action;
                }
                dbi.setSetting("agent_rules", csv);
                agenttools::saveAgentConfig(cfg);
                st.statusNote = "agent permit: " + a.positional[1] + " "
                              + a.positional[2] + " " + act;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "sessions") {
                // The named sessions = the ASCII tabs: switch between
                // the independent agent conversations.
                std::string act = a.positional.size() >= 2 ? a.positional[1] : "";
                static std::string activeName;   // "" = the main history
                if (act == "new" && a.positional.size() >= 3) {
                    activeName = a.positional[2];
                    std::vector<agenttools::Message> fresh;
                    agenttools::saveSession(".agent-sessions/session-"
                                            + activeName + ".json", fresh);
                    st.statusNote = "session: " + activeName;
                } else if (act == "switch" && a.positional.size() >= 3) {
                    // Save the current one, load the target.
                    if (!activeName.empty()) {
                        agenttools::saveSession(".agent-sessions/session-"
                                                + activeName + ".json",
                                                agentHistory);
                    }
                    activeName = a.positional[2];
                    agentHistory.clear();
                    agenttools::loadSession(".agent-sessions/session-"
                                            + activeName + ".json",
                                            agentHistory);
                    st.statusNote = "session: " + activeName;
                } else if (act == "list" || act.empty()) {
                    glob_t g{};
                    if (glob(".agent-sessions/session-*.json", 0, nullptr, &g) == 0) {
                        for (size_t i = 0; i < g.gl_pathc; ++i) {
                            std::string p = g.gl_pathv[i];
                            auto a = p.find("session-");
                            auto b = p.rfind(".json");
                            std::string nm = p.substr(a + 8,
                                                      b == std::string::npos
                                                          ? std::string::npos
                                                          : b - a - 8);
                            std::cout << (nm == activeName ? "  * " : "    ")
                                      << nm << std::endl;
                        }
                        globfree(&g);
                    } else {
                        std::cout << "  (no sessions — 'agent sessions new <name>')"
                                  << std::endl;
                    }
                } else {
                    std::cout << "Usage: agent sessions new|switch|list [name]"
                              << std::endl;
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "session") {
                // agent session save|load|list [name]
                std::string act = a.positional.size() >= 2 ? a.positional[1] : "";
                std::string name = a.positional.size() >= 3 ? a.positional[2] : "";
                std::string dir = ".agent-sessions";
                if (act == "save" && !name.empty()) {
                    mkdir(dir.c_str(), 0755);
                    agenttools::saveSession(dir + "/" + name + ".json",
                                            agentHistory);
                    st.statusNote = "session saved: " + name;
                } else if (act == "load" && !name.empty()) {
                    if (agenttools::loadSession(dir + "/" + name + ".json",
                                                agentHistory)) {
                        st.statusNote = "session loaded: " + name;
                    } else {
                        std::cout << "no such session: " << name << std::endl;
                    }
                } else if (act == "export" && !name.empty()) {
                    std::string dest = a.positional.size() >= 4
                                           ? a.positional[3] : name + ".json";
                    agenttools::saveSession(dest, agentHistory);
                    st.statusNote = "session exported to " + dest;
                } else if (act == "import" && !name.empty()) {
                    std::string src = name;
                    if (agenttools::loadSession(src, agentHistory)) {
                        std::string base = src;
                        auto slash = base.rfind('/');
                        if (slash != std::string::npos) base = base.substr(slash + 1);
                        if (base.size() > 5 && base.substr(base.size() - 5) == ".json") {
                            base = base.substr(0, base.size() - 5);
                        }
                        mkdir(dir.c_str(), 0755);
                        agenttools::saveSession(dir + "/" + base + ".json",
                                                agentHistory);
                        st.statusNote = "session imported as " + base;
                    } else {
                        std::cout << "cannot import: " << src << std::endl;
                    }
                } else if (act == "list" || act.empty()) {
                    glob_t g{};
                    if (glob((dir + "/*.json").c_str(), 0, nullptr, &g) == 0) {
                        for (size_t i = 0; i < g.gl_pathc; ++i) {
                            std::cout << "  " << g.gl_pathv[i] << std::endl;
                        }
                        globfree(&g);
                    } else {
                        std::cout << "  (no saved sessions)" << std::endl;
                    }
                } else {
                    std::cout << "Usage: agent session save|load|list [name]"
                              << std::endl;
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "compact") {
                // "compact here N": keep the last N user turns, the head
                // becomes one summary note (the hermes partial compress).
                int keepTurns = -1;
                if (a.positional.size() >= 3 && a.positional[1] == "here") {
                    try { keepTurns = std::stoi(a.positional[2]); } catch (...) {}
                }
                if (keepTurns > 0) {
                    // Count the user turns from the end; the tail starts
                    // at the (N+1)-th user message from the end.
                    int userSeen = 0;
                    int cut = static_cast<int>(agentHistory.size());
                    for (int i = static_cast<int>(agentHistory.size()) - 1;
                         i >= 0; --i) {
                        if (agentHistory[static_cast<size_t>(i)].role == "user") {
                            if (++userSeen > keepTurns) break;
                            cut = i;
                        }
                    }
                    std::vector<agenttools::Message> tail(
                        agentHistory.begin() + cut, agentHistory.end());
                    agenttools::Message note;
                    note.role = "user";
                    note.content = "[CONTEXT COMPACTION: the earlier turns are "
                                   "summarised away — continue from here]";
                    agentHistory.clear();
                    agentHistory.push_back(note);
                    for (auto& m : tail) agentHistory.push_back(m);
                    st.statusNote = "history compacted to the last "
                                  + std::to_string(keepTurns) + " turns";
                    return 1;
                }
                // The default: drop the old tool results (the codex prune).
                int kept = 0;
                for (auto it = agentHistory.rbegin(); it != agentHistory.rend();
                     ++it) {
                    if (++kept > 8 && it->role == "tool") {
                        it->content = "(old tool result removed)";
                    }
                }
                st.statusNote = "history compacted";
                return 1;
            }
            if (!a.positional.empty() &&
                (a.positional[0] == "pause" || a.positional[0] == "resume")) {
                const char* estop = ".agent-estop";
                if (a.positional[0] == "pause") {
                    std::ofstream f(estop, std::ios::trunc);
                    f << "paused";
                    st.statusNote = "agent paused (e-stop)";
                } else {
                    std::remove(estop);
                    st.statusNote = "agent resumed";
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "cron") {
                // agent cron add <spec> <prompt...> | list | remove <id> | check
                const std::string cronPath = ".agent-cron/jobs.json";
                std::vector<agenttools::CronJob> jobs;
                agenttools::loadCronJobs(cronPath, jobs);
                std::string act = a.positional.size() >= 2 ? a.positional[1] : "";
                if (act == "add" && a.positional.size() >= 4) {
                    int64_t next = agenttools::nextRunFromSpec(
                        a.positional[2], static_cast<int64_t>(std::time(nullptr)));
                    if (next < 0) {
                        std::cout << "bad schedule: " << a.positional[2]
                                  << " (try '30m', 'every 2h', a cron expr,"
                                     " or YYYY-MM-DD)" << std::endl;
                        return 1;
                    }
                    std::string prompt;
                    for (size_t i = 3; i < a.positional.size(); ++i) {
                        prompt += (prompt.empty() ? "" : " ") + a.positional[i];
                    }
                    agenttools::CronJob j;
                    j.id = "job_" + std::to_string(std::time(nullptr))
                         + "_" + std::to_string(jobs.size());
                    j.spec = a.positional[2];
                    j.prompt = prompt;
                    j.nextRun = next;
                    jobs.push_back(j);
                    mkdir(".agent-cron", 0755);
                    agenttools::saveCronJobs(cronPath, jobs);
                    st.statusNote = "cron added: " + j.spec;
                } else if (act == "list" || act.empty()) {
                    for (const auto& j : jobs) {
                        std::time_t t = static_cast<std::time_t>(j.nextRun);
                        std::tm tm{};
                        localtime_r(&t, &tm);
                        char buf[32];
                        std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tm);
                        std::cout << "  " << j.id << "  " << j.spec << "  next "
                                  << buf << "  " << j.prompt << std::endl;
                    }
                    if (jobs.empty()) std::cout << "  (no cron jobs)" << std::endl;
                } else if (act == "remove" && a.positional.size() >= 3) {
                    std::vector<agenttools::CronJob> kept;
                    for (const auto& j : jobs) {
                        if (j.id != a.positional[2]) kept.push_back(j);
                    }
                    jobs = kept;
                    agenttools::saveCronJobs(cronPath, jobs);
                    st.statusNote = "cron removed: " + a.positional[2];
                } else if (act == "check") {
                    int64_t now = static_cast<int64_t>(std::time(nullptr));
                    bool any = false;
                    for (auto& j : jobs) {
                        if (j.nextRun > now) continue;
                        if (agenttools::eStopEngaged()) {
                            std::cout << "  cron paused (the e-stop)" << std::endl;
                            break;
                        }
                        any = true;
                        std::cout << "== cron " << j.id << ": " << j.prompt
                                  << std::endl;
                        auto escW4 = startEscWatcher();
                        agenttools::Result res = agenttools::run(
                            cfg, j.prompt, agentHistory, confirm, ask, log,
                            [](const std::string& t) {
                                std::cout << t << std::flush;
                            });
                        std::cout << (res.ok ? res.text
                                             : "[agent error] " + res.error)
                                  << std::endl;
                        stopEscWatcher(escW4);
                        j.nextRun = agenttools::nextRunFromSpec(j.spec, now);
                        if (j.nextRun < 0) j.nextRun = now + 86400;
                    }
                    if (any) agenttools::saveCronJobs(cronPath, jobs);
                    else st.statusNote = "cron check: nothing due";
                } else {
                    std::cout << "Usage: agent cron add <spec> <prompt> |"
                                 " list | remove <id> | check" << std::endl;
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "undo") {
                int n = 1;
                if (a.positional.size() >= 2) {
                    try { n = std::stoi(a.positional[1]); } catch (...) {}
                }
                int removed = agenttools::undoLastTurns(agentHistory, n);
                st.statusNote = "undo: removed " + std::to_string(removed)
                              + " messages";
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "edit") {
                // agent edit [N] <text> — replace the Nth user message
                // (the last by default) and drop everything after it.
                int n = 0;
                size_t from = 1;
                if (a.positional.size() >= 2 &&
                    std::isdigit(static_cast<unsigned char>(a.positional[1][0]))) {
                    try {
                        n = std::stoi(a.positional[1]);
                        from = 2;
                    } catch (...) {
                        from = 1;
                    }
                }
                std::string text;
                for (size_t i = from; i < a.positional.size(); ++i) {
                    text += (text.empty() ? "" : " ") + a.positional[i];
                }
                if (text.empty()) {
                    std::cout << "Usage: agent edit [N] <new text>" << std::endl;
                    return 1;
                }
                int userSeen = 0;
                int target = -1;
                for (int i = static_cast<int>(agentHistory.size()) - 1; i >= 0; --i) {
                    if (agentHistory[static_cast<size_t>(i)].role == "user") {
                        if (++userSeen >= (n == 0 ? 1 : n)) { target = i; break; }
                    }
                }
                if (target < 0) {
                    st.statusNote = "no user message to edit";
                    return 1;
                }
                agentHistory.resize(static_cast<size_t>(target + 1));
                agentHistory[static_cast<size_t>(target)].content = text;
                st.statusNote = "edited — the last answer is dropped";
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "title") {
                if (cfg.key.empty()) {
                    std::cout << "no API key — the title needs the LLM"
                              << std::endl;
                    return 1;
                }
                std::vector<agenttools::Message> tHist;
                for (const auto& m : agentHistory) {
                    if (!m.content.empty() && m.role != "tool") {
                        tHist.push_back({m.role, m.content.substr(0, 300),
                                         {}, "", ""});
                    }
                }
                tHist.push_back({"user",
                                 "Generate a short title (3-5 words, no quotes) "
                                 "for this conversation.", {}, "", ""});
                agenttools::Result tr = agenttools::run(
                    cfg, "name the conversation", tHist, nullptr, nullptr, log,
                    nullptr);
                std::string title = tr.ok ? tr.text : "";
                auto b = title.find_first_not_of(" \"\n");
                if (b == std::string::npos) b = 0;
                auto e = title.find_last_not_of(" \"\n");
                title = title.substr(b, e == std::string::npos
                                            ? std::string::npos : e - b + 1);
                if (!title.empty()) {
                    std::ofstream tf(".agent-sessions/last.title",
                                     std::ios::trunc);
                    tf << title;
                }
                st.statusNote = "title: " + title;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "lsp") {
                // agent lsp hover|def <file>:<line>:<col>
                if (a.positional.size() < 3) {
                    std::cout << "Usage: agent lsp hover|def <file>:<line>:<col>"
                              << std::endl;
                    return 1;
                }
                std::string op = a.positional[1];
                if (op == "def") op = "definition";
                std::string spec = a.positional[2];
                std::string path = spec;
                int line = 1, col = 1;
                auto c1 = spec.rfind(':');
                auto c2 = spec.rfind(':', c1 - 1);
                if (c1 != std::string::npos && c2 != std::string::npos) {
                    path = spec.substr(0, c2);
                    line = std::atoi(spec.c_str() + c2 + 1);
                    col = std::atoi(spec.c_str() + c1 + 1);
                } else if (c1 != std::string::npos) {
                    path = spec.substr(0, c1);
                    line = std::atoi(spec.c_str() + c1 + 1);
                }
                if (op != "hover" && op != "definition") {
                    std::cout << "the operation must be hover|def" << std::endl;
                    return 1;
                }
                std::cout << agenttools::lspQueryPublic(cfg, op, path, line,
                                                        col) << std::endl;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "usage") {
                std::cout << "  " << agenttools::agentUsageLine() << std::endl;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "providers") {
                for (const auto& p : agenttools::providerPresets()) {
                    std::cout << "  " << p.name << "  →  " << p.provider
                              << " " << p.endpoint << " / " << p.model
                              << std::endl;
                }
                std::cout << "  (agent config provider <name> applies one)"
                          << std::endl;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "reasoning") {
                std::string v = a.positional.size() >= 2 ? a.positional[1] : "";
                if (v != "low" && v != "medium" && v != "high") {
                    std::cout << "Usage: agent reasoning low|medium|high"
                              << std::endl;
                    return 1;
                }
                cfg.reasoning = v;
                agenttools::saveAgentConfig(cfg);
                st.statusNote = "reasoning: " + v;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "skills") {
                glob_t g{};
                std::string dir = ".agent-skills";
                mkdir(dir.c_str(), 0755);
                if (glob((dir + "/*.md").c_str(), 0, nullptr, &g) == 0) {
                    for (size_t i = 0; i < g.gl_pathc; ++i) {
                        std::cout << "  " << g.gl_pathv[i] << std::endl;
                    }
                    globfree(&g);
                } else {
                    std::cout << "  (no skills yet — the agent can create"
                                 " .agent-skills/<name>.md files)" << std::endl;
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "proxy") {
                std::string v = a.positional.size() >= 2 ? a.positional[1] : "";
                if (v == "off") v = "";
                dbi.setSetting("agent_proxy", v);
            cfg.proxy = v;
            agenttools::saveAgentConfig(cfg);
                st.statusNote = v.empty() ? "agent proxy: off"
                                          : "agent proxy: " + v;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "sandbox") {
                std::string v = a.positional.size() >= 2 ? a.positional[1] : "";
                if (v != "off" && v != "bwrap") {
                    std::cout << "Usage: agent sandbox off|bwrap" << std::endl;
                    return 1;
                }
                dbi.setSetting("agent_sandbox", v);
            cfg.sandbox = v;
            agenttools::saveAgentConfig(cfg);
                st.statusNote = "agent sandbox: " + v;
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "mcp") {
                // agent mcp add <name> <command> | list | del <name>
                std::string act = a.positional.size() >= 2 ? a.positional[1] : "";
                if (act == "list" || act.empty()) {
                    for (const auto& m : cfg.mcpServers) {
                        std::cout << "  " << m.name << "  →  " << m.command
                                  << std::endl;
                    }
                } else if (act == "add" && a.positional.size() >= 4) {
                    std::string cmdline;
                    for (size_t i = 3; i < a.positional.size(); ++i) {
                        cmdline += (cmdline.empty() ? "" : " ") + a.positional[i];
                    }
                    cfg.mcpServers.push_back({a.positional[2], cmdline});
                    std::string csv;
                    for (const auto& m : cfg.mcpServers) {
                        csv += (csv.empty() ? "" : ",") + m.name + "|" + m.command;
                    }
                    dbi.setSetting("agent_mcp", csv);
                    agenttools::saveAgentConfig(cfg);
                    st.statusNote = "agent mcp: " + a.positional[2] + " added";
                } else if (act == "del" && a.positional.size() >= 3) {
                    std::vector<agenttools::McpServer> kept;
                    for (const auto& m : cfg.mcpServers) {
                        if (m.name != a.positional[2]) kept.push_back(m);
                    }
                    cfg.mcpServers = kept;
                    std::string csv;
                    for (const auto& m : cfg.mcpServers) {
                        csv += (csv.empty() ? "" : ",") + m.name + "|" + m.command;
                    }
                    dbi.setSetting("agent_mcp", csv);
                    agenttools::saveAgentConfig(cfg);
                    st.statusNote = "agent mcp: " + a.positional[2] + " removed";
                } else {
                    std::cout << "Usage: agent mcp add <name> <command> |"
                                 " agent mcp list | agent mcp del <name>" << std::endl;
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "goal") {
                // agent goal <text> | draft <objective> | show | status |
                //        pause | resume | clear | gate <cmd> | check
                std::string act = a.positional.size() >= 2 ? a.positional[1] : "";
                if (act == "draft" && a.positional.size() >= 3) {
                    std::string objective;
                    for (size_t i = 2; i < a.positional.size(); ++i) {
                        objective += (objective.empty() ? "" : " ") + a.positional[i];
                    }
                    cfg.goal.goal = objective;
                    cfg.goal.contract = agenttools::draftContract(cfg, objective);
                    cfg.goal.achieved = false;
                    cfg.goal.paused = false;
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "goal set with the contract";
                } else if (act == "show" || act == "status") {
                    if (cfg.goal.goal.empty()) {
                        std::cout << "  (no active goal)" << std::endl;
                    } else {
                        std::cout << "  goal: " << cfg.goal.goal
                                  << (cfg.goal.paused ? " [paused]" : "")
                                  << (cfg.goal.achieved ? " [achieved]" : "")
                                  << std::endl;
                        if (!cfg.goal.contract.empty()) {
                            std::cout << "  contract:\n" << cfg.goal.contract
                                      << std::endl;
                        }
                        if (!cfg.goal.subgoals.empty()) {
                            std::cout << "  subgoals:" << std::endl;
                            for (size_t i = 0; i < cfg.goal.subgoals.size(); ++i) {
                                std::cout << "    " << i + 1 << ". "
                                          << cfg.goal.subgoals[i] << std::endl;
                            }
                        }
                        if (!cfg.goal.gateCommand.empty()) {
                            std::cout << "  gate: " << cfg.goal.gateCommand
                                      << std::endl;
                        }
                    }
                } else if (act == "pause") {
                    cfg.goal.paused = true;
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "goal paused";
                } else if (act == "resume") {
                    cfg.goal.paused = false;
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "goal resumed";
                } else if (act == "clear") {
                    cfg.goal = agenttools::GoalState();
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "goal cleared";
                } else if (act == "gate" && a.positional.size() >= 3) {
                    cfg.goal.gateCommand = a.positional[2];
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "goal gate: " + cfg.goal.gateCommand;
                } else if (act == "check") {
                    if (cfg.goal.goal.empty()) {
                        std::cout << "  (no active goal)" << std::endl;
                    } else {
                        std::string verdict = agenttools::judgeGoal(
                            cfg, cfg.goal, agentHistory, log);
                        std::cout << "  " << verdict << std::endl;
                        if (verdict.rfind("goal achieved", 0) == 0) {
                            cfg.goal.achieved = true;
                            agenttools::saveGoal(".agent-goal.json", cfg.goal);
                        }
                    }
                } else if (a.positional.size() >= 2) {
                    // The bare goal text.
                    std::string text;
                    for (size_t i = 1; i < a.positional.size(); ++i) {
                        text += (text.empty() ? "" : " ") + a.positional[i];
                    }
                    cfg.goal.goal = text;
                    cfg.goal.contract.clear();
                    cfg.goal.achieved = false;
                    cfg.goal.paused = false;
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "goal set";
                } else {
                    std::cout << "Usage: agent goal <text> | draft <objective> |"
                                 " show | pause | resume | clear | gate <cmd> |"
                                 " check" << std::endl;
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "subgoal") {
                std::string arg;
                for (size_t i = 1; i < a.positional.size(); ++i) {
                    arg += (arg.empty() ? "" : " ") + a.positional[i];
                }
                if (cfg.goal.goal.empty()) {
                    std::cout << "  no active goal — set one with 'agent goal'"
                              << std::endl;
                    return 1;
                }
                if (arg.empty()) {
                    for (size_t i = 0; i < cfg.goal.subgoals.size(); ++i) {
                        std::cout << "  " << i + 1 << ". "
                                  << cfg.goal.subgoals[i] << std::endl;
                    }
                    if (cfg.goal.subgoals.empty()) {
                        std::cout << "  (no subgoals)" << std::endl;
                    }
                } else if (arg == "clear") {
                    cfg.goal.subgoals.clear();
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "subgoals cleared";
                } else if (arg.rfind("remove ", 0) == 0) {
                    try {
                        int n = std::stoi(arg.substr(7));
                        if (n < 1 || n > static_cast<int>(cfg.goal.subgoals.size())) {
                            std::cout << "  no subgoal #" << n << std::endl;
                        } else {
                            cfg.goal.subgoals.erase(cfg.goal.subgoals.begin() + n - 1);
                            agenttools::saveGoal(".agent-goal.json", cfg.goal);
                            st.statusNote = "subgoal " + std::to_string(n) + " removed";
                        }
                    } catch (...) {
                        std::cout << "Usage: agent subgoal remove <n>" << std::endl;
                    }
                } else {
                    cfg.goal.subgoals.push_back(arg);
                    agenttools::saveGoal(".agent-goal.json", cfg.goal);
                    st.statusNote = "subgoal added";
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "log") {
                // agent log [N] — the last messages of the conversation.
                int n = 10;
                if (a.positional.size() >= 2) {
                    try { n = std::stoi(a.positional[1]); } catch (...) {}
                }
                int start = std::max(0, static_cast<int>(agentHistory.size()) - n);
                for (int i = start; i < static_cast<int>(agentHistory.size()); ++i) {
                    const auto& m = agentHistory[static_cast<size_t>(i)];
                    std::string tag = m.role == "user" ? "user "
                                    : m.role == "assistant" ? "agent"
                                    : "tool ";
                    std::string body = m.content.empty() && !m.calls.empty()
                                           ? "(tool calls: " +
                                                 m.calls[0].name + ")"
                                           : m.content;
                    std::cout << "  [" << tag << "] " << body << std::endl;
                }
                return 1;
            }
            if (!a.positional.empty() && a.positional[0] == "reset") {
                agentHistory.clear();
                st.statusNote = "agent history cleared";
                return 1;
            }
            if (cfg.key.empty()) {
                std::cout << "No API key: set it with 'agent config key <key>',"
                             " 'agent config provider openai|anthropic' or the"
                             " OPENAI_API_KEY / ANTHROPIC_API_KEY env vars."
                          << std::endl;
                return 1;
            }
            if (a.positional.empty()) {
                std::cout << "agent (" << cfg.provider << ", " << cfg.model
                          << ", trust: " << cfg.trust
                          << "). Prompts below; 'exit' ends, 'reset' clears."
                          << std::endl;
                std::string line;
                for (;;) {
                    if (!readLineWithHistory(agentPromptHistory, "agent> ", line)) break;
                    auto b = line.find_first_not_of(" \t");
                    if (b == std::string::npos) continue;
                    auto e = line.find_last_not_of(" \t");
                    line = line.substr(b, e - b + 1);
                    if (line == "exit" || line == "quit") break;
                    if (line == "reset") { agentHistory.clear(); continue; }
                    auto escW2 = startEscWatcher();
                    agenttools::Result res =
                        agenttools::run(cfg, line, agentHistory, confirm, ask, log,
                              [](const std::string& t) {
                                  std::cout << t << std::flush;
                              });
                    if (!res.ok) std::cout << "[agent error] " << res.error << std::endl;
                    else if (!res.streamed) std::cout << res.text << std::endl;
                    stopEscWatcher(escW2);
                }
                return 1;
            }
            std::string prompt;
            size_t from = 0;
            auto expandMentions = [](const std::string& in) -> std::string {
                // The @path mentions insert the file content (truncated).
                std::string out;
                size_t i = 0;
                while (i < in.size()) {
                    if (in[i] == '@' && (i == 0 || in[i - 1] == ' ')) {
                        size_t j = i + 1;
                        while (j < in.size() && !std::isspace(
                                    static_cast<unsigned char>(in[j]))) j++;
                        std::string path = in.substr(i + 1, j - i);
                        std::ifstream f(path, std::ios::binary);
                        if (f) {
                            std::ostringstream ss;
                            ss << f.rdbuf();
                            std::string body = ss.str();
                            if (body.size() > 4000) {
                                body = body.substr(0, 4000) + "\n...(truncated)";
                            }
                            out += "\n--- " + path + " ---\n" + body
                                 + "\n--- end " + path + " ---\n";
                        } else {
                            out += "@" + path;
                        }
                        i = j;
                        continue;
                    }
                    out += in[i];
                    i++;
                }
                return out;
            };
            bool planRun = !a.positional.empty() && a.positional[0] == "plan";
            if (planRun) from = 1;
            for (size_t i = from; i < a.positional.size(); ++i) {
                prompt += (prompt.empty() ? "" : " ") + a.positional[i];
            }
            if (planRun && prompt.empty()) {
                std::cout << "Usage: agent plan <prompt>" << std::endl;
                return 1;
            }
            prompt = expandMentions(prompt);
            if (planRun) {
                mkdir(".agent-plans", 0755);
                std::time_t now = std::time(nullptr);
                char ts[32];
                std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S",
                              localtime(&now));
                std::string planFile = std::string(".agent-plans/plan-") + ts
                                     + ".md";
                agenttools::Config planCfg = cfg;
                planCfg.planMode = true;
                planCfg.planFile = planFile;
                std::vector<agenttools::Message> planHist;
                auto escW3 = startEscWatcher();
                agenttools::Result pr = agenttools::run(
                    planCfg, prompt, planHist, confirm, ask, log);
                std::cout << (pr.ok ? pr.text : "[agent error] " + pr.error)
                          << std::endl;
                stopEscWatcher(escW3);
                if (!pr.ok) return 1;
                std::cout << "plan: " << planFile << " — execute now? [y/N] "
                          << std::flush;
                std::string ans;
                std::getline(std::cin, ans);
                if (ans == "y" || ans == "Y") {
                    std::string execPrompt =
                        "Execute the approved plan. First read " + planFile
                        + " and then carry out the steps in it.";
                    auto escW5 = startEscWatcher();
                    agenttools::Result res = agenttools::run(
                        cfg, execPrompt, agentHistory, confirm, ask, log);
                    if (!res.ok) std::cout << "[agent error] " << res.error
                                           << std::endl;
                    else if (!res.streamed) std::cout << res.text << std::endl;
                    stopEscWatcher(escW5);
                }
                return 1;
            }
            auto escW = startEscWatcher();
            agenttools::Result res =
                agenttools::run(cfg, prompt, agentHistory, confirm, ask, log,
                              [](const std::string& t) {
                                  std::cout << t << std::flush;
                              });
            if (!res.ok) {
                std::cout << "[agent error] " << res.error << std::endl;
            } else if (!res.streamed) {
                std::cout << res.text << std::endl;
            }
            mkdir(".agent-sessions", 0755);
            agenttools::saveSession(".agent-sessions/last.json", agentHistory);
            stopEscWatcher(escW);
            return 1;
        }
        // ---- reply: send a reply to a message ----
    return 0;
}

} // namespace matrixcli

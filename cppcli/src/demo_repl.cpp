// src/demo_tui.cpp — the demo data, the demo REPL and the ncurses TUI
// (split out of main.cpp so the compilation stays incremental-friendly).
#include "commands.hpp"
#include "config.hpp"
#include "cli/args.hpp"
#include "core/http_client.hpp"
#include "globals.hpp"
#include "pcore.hpp"
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"
#include "../lib/util/string_utils.hpp"
#include "../lib/util/client_utils.hpp"
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/agent_setup.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace matrixcli;

#include "demo_tui.hpp"

extern int cmdRooms(const matrixcli::cli::Args& args);
extern int cmdView(const matrixcli::cli::Args& args);
extern int cmdSearch(const matrixcli::cli::Args& args);

// ---- Interactive demo REPL (offline, no Matrix account needed) ----
//
// Replaces the old `demo` behavior (which started an HTTP API server on
// port 8080). Now `progressive-cli demo` drops the user into an interactive
// terminal session against the offline demo database: type commands, see
// output, same handlers as the real CLI (rooms/view/search). The web demo
// stays available as `progressive-cli serve --demo`.

static void demoReplParseLine(const std::string& line, matrixcli::cli::Args& out) {
    std::istringstream iss(line);
    std::vector<std::string> words;
    std::string w;
    while (iss >> w) words.push_back(w);
    if (words.empty()) return;
    out.command = words[0];
    for (size_t i = 1; i < words.size(); ++i) {
        if (words[i].size() >= 2 && words[i][0] == '-' && words[i][1] == '-') {
            std::string key = words[i].substr(2);
            bool nextIsOpt = (i + 1 < words.size()) &&
                words[i + 1].size() >= 2 && words[i + 1][0] == '-' && words[i + 1][1] == '-';
            if (i + 1 < words.size() && !nextIsOpt) {
                out.options[key] = words[i + 1];
                ++i;
            } else {
                out.options[key] = "true";
            }
        } else {
            out.positional.push_back(words[i]);
        }
    }
}

static void demoReplSend(const matrixcli::cli::Args& args) {
    if (args.positional.size() < 2) {
        std::cout << "Usage: send <room> <message text>" << std::endl;
        return;
    }
    matrixcli::db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cout << "[demo] cannot open matrixcli.db" << std::endl;
        return;
    }
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string body;
    for (size_t i = 1; i < args.positional.size(); ++i) {
        if (!body.empty()) body += " ";
        body += args.positional[i];
    }
    matrixcli::matrix::Event ev;
    ev.event_id = "$demo_" + std::to_string(ts);
    ev.room_id = args.positional[0];
    ev.sender = "@you:demo.local";
    ev.type = "m.room.message";
    ev.content = {{"body", body}, {"msgtype", "m.text"}};
    ev.origin_server_ts = ts;
    dbi.insertEvent(ev);
    std::cout << "[demo] sent " << ts << " to " << args.positional[0] << ": " << body << std::endl;
}

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args& args);
#endif
int cmdDemoRepl(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    // Pure CLI mode: populate the demo DB and exit — the user then runs the
    // normal one-shot commands (progressive-cli rooms / view / send / search).
    auto runPureCli = []() {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (dbi.listRooms().empty()) populateDemoData(dbi);
        std::cout << "Demo data ready. Use the one-shot commands:\n"
                     "  progressive-cli rooms\n"
                     "  progressive-cli view \"#general\" 10\n"
                     "  progressive-cli send \"#general\" \"hello\"\n";
        return 0;
    };
    // The positional shortcuts: demo tui | ui | mobile | cli — no menu.
    if (!args.positional.empty()) {
        const std::string mode = args.positional[0];
        cli::Args sub = args;
        sub.positional.erase(sub.positional.begin());
        if (mode == "tui") {
            sub.options["tui"] = "true";
            return cmdDemoRepl(sub);
        }
        if (mode == "ui" || mode == "ascii") {
            sub.options["ui"] = "true";
            return cmdDemoRepl(sub);
        }
        if (mode == "mobile" || mode == "phone") {
            sub.options["ui"] = "true";
            sub.options["mobile"] = "true";
            return cmdDemoRepl(sub);
        }
        if (mode == "cli" || mode == "populate") {
            sub.options["cli"] = "true";
            return cmdDemoRepl(sub);
        }
    }

    if (args.options.count("cli") || args.options.count("populate")) {
        return runPureCli();
    }

    // Demo + ASCII interface: 'demo --ui [room]' runs the ASCII client
    // interface; with --static/--once it draws the frame once and exits
    // (non-interactive, pipe-friendly) instead of starting the REPL.
    if (args.options.count("ui") || args.options.count("ascii")) {
        cli::Args uiArgs;
        if (!args.positional.empty()) {
            uiArgs.positional.push_back(args.positional[0]);
        }
        if (args.options.count("static") || args.options.count("once")) {
            uiArgs.options["static"] = "true";
        }
        if (args.options.count("ids")) uiArgs.options["ids"] = "true";
        if (args.options.count("time-full") || args.options.count("sec"))
            uiArgs.options["time-full"] = "true";
        if (args.options.count("right")) uiArgs.options["right"] = args.options.at("right");
        if (args.options.count("thread-root"))
            uiArgs.options["thread-root"] = args.options.at("thread-root");
        if (args.options.count("thread"))
            uiArgs.options["thread"] = args.options.at("thread");
        if (args.options.count("limit")) uiArgs.options["limit"] = args.options.at("limit");
        if (args.options.count("rows")) uiArgs.options["rows"] = args.options.at("rows");
        if (args.options.count("scroll")) uiArgs.options["scroll"] = args.options.at("scroll");
        if (args.options.count("scroll-left")) uiArgs.options["scroll-left"] = args.options.at("scroll-left");
        if (args.options.count("mobile")) uiArgs.options["mobile"] = "true";
        if (args.options.count("panel-left")) uiArgs.options["panel-left"] = args.options.at("panel-left");
        if (args.options.count("panel-right")) uiArgs.options["panel-right"] = args.options.at("panel-right");
        if (args.options.count("panel-auto")) uiArgs.options["panel-auto"] = args.options.at("panel-auto");
        if (args.options.count("members")) uiArgs.options["members"] = args.options.at("members");
        if (args.options.count("space")) uiArgs.options["space"] = args.options.at("space");
        if (args.options.count("time-side")) uiArgs.options["time-side"] = args.options.at("time-side");
        if (args.options.count("msg-line")) uiArgs.options["msg-line"] = args.options.at("msg-line");
        for (const char* k : {"agent", "agent-provider", "agent-endpoint",
                              "agent-model", "agent-key", "agent-trust"}) {
            if (args.options.count(k)) uiArgs.options[k] = args.options.at(k);
        }
        return matrixcli::cmdAsciiUi(uiArgs);
    }

    // Demo + the ncurses TUI: 'demo --tui' (or choice 4) — the terminal UI
    // with the demo database populated.
    if (args.options.count("tui")) {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (dbi.listRooms().empty()) populateDemoData(dbi);
#ifdef BUILD_TUI
        return cmdTUI(args);
#else
        std::cout << "tui not built (recompile with BUILD_TUI=ON)." << std::endl;
        return 1;
#endif
    }

    // On a terminal, let the user CHOOSE: interactive REPL, pure CLI, or
    // the ASCII-drawn client interface (rooms | chat | members).
    if (isatty(STDIN_FILENO)) {
        std::cout << "Choose demo mode:\n"
                     "  1) interactive session (type commands at a prompt)\n"
                     "  2) populate demo data and exit (one-shot commands)\n"
                     "  3) ASCII client interface (rooms | chat | members)\n"
                     "     (non-interactive: progressive-cli demo --ui --static)\n"
                     "  4) terminal UI (ncurses TUI)\n"
                     "  5) ASCII client for smartphones (stacked, portrait)\n"
                     "Choice [1/2/3/4/5]: " << std::flush;
        std::string ans;
        std::getline(std::cin, ans);
        if (!matrixcli::g_interrupted.load()) {
            // Ctrl+C at the choice prompt — leave immediately.
            std::cout << "Bye!" << std::endl;
            return 0;
        }
        if (!ans.empty() && (ans[0] == '2' || ans[0] == 'n')) {
            return runPureCli();
        }
        if (!ans.empty() && ans[0] == '3') {
            cli::Args uiArgs;
            return matrixcli::cmdAsciiUi(uiArgs);
        }
        if (!ans.empty() && ans[0] == '5') {
            cli::Args uiArgs;
            uiArgs.options["mobile"] = "true";
            return matrixcli::cmdAsciiUi(uiArgs);
        }
        if (!ans.empty() && ans[0] == '4') {
            cli::Args tuiArgs;
            tuiArgs.options["tui"] = "true";
            return cmdDemoRepl(tuiArgs);
        }
    }

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;
    if (dbi.listRooms().empty()) {
        std::cout << "Populating demo data..." << std::endl;
        populateDemoData(dbi);
    }

    std::cout << "progressive-cli demo — interactive mode (offline, no account needed)" << std::endl;
    std::cout << "Commands: help | rooms | view <room> [n] | search <query> |"
              << " send <room> <text> | ui | clear | quit" << std::endl;
    std::cout << "Demo rooms: #general  #dev  #random  #dm_alice  #dm_bob" << std::endl;

    std::string line;
    std::vector<std::string> history;
    for (;;) {
        if (!matrixcli::g_interrupted.load()) break;  // Ctrl+C
        if (!readLineWithHistory(history, "demo> ", line)) break;
        if (!matrixcli::g_interrupted.load()) break;
        // trim
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        cli::Args a;
        demoReplParseLine(line, a);

        if (a.command == "quit" || a.command == "exit") break;
        if (a.command == "help") {
            std::cout << "  rooms                     list demo rooms\n"
                         "  view <room> [n]           show the last n messages (default 20)\n"
                         "  search <query>            full-text search in cached messages\n"
                         "  send <room> <text>        send a message (demo, offline)\n"
                         "  ui                        ASCII client interface (rooms | chat | members)\n"
                         "  clear                     clear the screen\n"
                         "  quit / exit               leave the demo\n";
            continue;
        }
        if (a.command == "clear") {
            std::cout << "\033[2J\033[H" << std::flush;
            continue;
        }
        if (a.command == "rooms") { cmdRooms(a); continue; }
        if (a.command == "view") { cmdView(a); continue; }
        if (a.command == "search") { cmdSearch(a); continue; }
        if (a.command == "send") { demoReplSend(a); continue; }
        if (a.command == "attach" || a.command == "send-file") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: attach <room> <file> [--caption text]" << std::endl;
                continue;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == roomQ || name == roomQ ||
                    (!roomQ.empty() && (name.find(roomQ) == 0 ||
                                        id.find(roomQ) != std::string::npos))) {
                    roomId = id;
                    break;
                }
            }
            std::string cap = a.options.count("caption") ? a.options.at("caption") : "";
            std::string thr = a.options.count("thread") ? a.options.at("thread") : "";
            uiInsertLocalFile(dbi, roomId, a.positional[1], cap, thr);
            std::cout << "[demo] file recorded locally: " << a.positional[1]
                      << " -> " << roomId << std::endl;
            continue;
        }
        if (a.command == "ui" || a.command == "ascii") {
            int rc = matrixcli::cmdAsciiUi(a);
            std::cout << "Back in the demo session (type 'quit' to leave)." << std::endl;
            if (rc != 0) continue;
            continue;
        }
        std::cout << "Unknown command '" << a.command << "' — type 'help'." << std::endl;
    }
    std::cout << "Bye!" << std::endl;
    return 0;
}

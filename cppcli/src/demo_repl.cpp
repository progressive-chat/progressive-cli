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
#include "ascii_ui_impl.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"
#include "../lib/util/string_utils.hpp"
#include "../lib/util/layout_remap.hpp"
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
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

using namespace matrixcli;

#include "demo_tui.hpp"
#include "demo_showcases.hpp"

extern int cmdRooms(const matrixcli::cli::Args& args);
extern int cmdView(const matrixcli::cli::Args& args);
extern int cmdSearch(const matrixcli::cli::Args& args);
extern int cmdPower(const matrixcli::cli::Args& args);

// Offline room info against the demo DB — mirrors the public `info` command,
// so `matrixcli demo general info` reproduces the room-info output with no
// account and no network.
static int demoReplInfo(db::Database& dbi, const cli::Args& a) {
    if (a.positional.empty()) {
        std::cout << "Usage: info <room>" << std::endl;
        return 1;
    }
    std::string roomQ = a.positional[0];
    std::string roomId = matchRoomInCache(dbi.listRooms(), roomQ);
    if (roomId.empty() && !roomQ.empty() && roomQ[0] == '#' &&
        roomQ.find(':') == std::string::npos)
        roomId = matchRoomInCache(dbi.listRooms(), roomQ.substr(1));
    if (roomId.empty()) roomId = roomQ;
    auto rooms = dbi.listRooms();
    for (auto& r : rooms) {
        if (r.value("room_id", "") == roomId) {
            printRoomInfo(&dbi, r);
            return 0;
        }
    }
    std::cout << "Room not found: " << roomQ << "\n";
    return 1;
}

// === Offline demo showcases: members / typing / edit / report / topic / threads ===
// Shared pattern: pick a room (and usually a message), then show a stylized
// dialog that mirrors the real command's behaviour — all against the offline
// demo DB, no account or network needed.

static void demoReplParseLine(const std::string& line, matrixcli::cli::Args& out) {
    std::istringstream iss(line);
    std::vector<std::string> words;
    std::string w;
    while (iss >> w) words.push_back(w);
    if (words.empty()) return;
    out.command = words[0];
    // Wrong keyboard layout: if the typed command isn't recognized but its
    // layout-swapped form is, use the swapped form (toggle: fuzzy_layout).
    if (demoFuzzyLayout()) {
        static const char* kDemoCmds[] = {
            "quit", "exit", "help", "clear", "rooms", "view", "info", "power",
            "perms", "search", "send", "markdown", "md", "vote", "voting",
            "attach", "send-file", "ui", "ascii", "members", "typing", "edit",
            "report", "topic", "threads", "config", "backup", "accounts",
            "tui", "mobile", "cli", "populate", "demo", "status", "profile",
            nullptr};
        auto known = [](const std::string& s) {
            for (int i = 0; kDemoCmds[i]; ++i)
                if (s == kDemoCmds[i]) return true;
            return false;
        };
        if (!known(out.command)) {
            std::string r = matrixcli::util::keyboardLayoutRemap(out.command);
            if (r != out.command && known(r)) out.command = r;
        }
    }
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

// The markdown rendering demo: the same sample that also lives in the
// #general demo room, rendered by the same ANSI renderer the chat view uses
// (renderMarkdownBody in ascii_ui_helpers.cpp).
#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args& args);
#endif

// Demo for the `accounts` command: a guided, non-destructive walkthrough of
// multi-account management (the vendored session store + config.json). It shows
// sample output and every flag without ever touching the real session.db or
// config.json, so it is safe to run anywhere.
void demoAccountsShowcase() {
    const char* a = ANSI_BOLD "\033[37m";
    const char* d = ANSI_DIM;
    const char* r = ANSI_RESET;
    std::cout << "Multi-account demo — " << a << "progressive-cli accounts" << r << "\n\n"
              << "Lists every logged-in Matrix account the client knows about "
                 "(the vendored session store),\nmarks the one you are using now "
                 "as " << a << "[active]" << r << ", and lets you hide accounts "
                 "you no longer want to see.\n\n"
              << "Example output:\n\n"
              << "  @alice:matrix.org (ALICEDEV) @ https://matrix.org" << a
              << " [active]" << r << "\n"
              << "  @bob:example.com (BOBPHONE) @ https://example.com\n"
              << "  @carol:matrix.org (CAROLXS) @ https://matrix.org" << d
              << " (hidden)" << r << "\n\n"
              << "Flags:\n"
              << "  accounts                               list visible accounts (with [active])\n"
              << "  accounts --all                         include the (hidden) ones too\n"
              << "  accounts --json                        machine-readable list\n"
              << "  accounts --hide @carol:matrix.org      hide permanently (config.json)\n"
              << "  accounts --show @carol:matrix.org      un-hide\n"
              << "  accounts --temporary-hide @bob:example.com   hide for this run only\n\n"
              << "Machine-readable form (" << a << "accounts --json" << r << "):\n\n"
              << "  [\n"
              << "    {\"user_id\":\"@alice:matrix.org\",\"device_id\":\"ALICEDEV\",\n"
              << "     \"homeserver_url\":\"https://matrix.org\",\"active\":true,\"hidden\":false},\n"
              << "    {\"user_id\":\"@bob:example.com\",\"device_id\":\"BOBPHONE\",\n"
              << "     \"homeserver_url\":\"https://example.com\",\"active\":false,\"hidden\":false},\n"
              << "    {\"user_id\":\"@carol:matrix.org\",\"device_id\":\"CAROLXS\",\n"
              << "     \"homeserver_url\":\"https://matrix.org\",\"active\":false,\"hidden\":true}\n"
              << "  ]\n\n"
              << "Run " << a << "progressive-cli accounts" << r
              << " to see your real accounts, or " << a << "login" << r
              << " to add one.\n";
}

int cmdDemoRepl(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    // Load the user config so demo-local toggles (e.g. fuzzy_layout) apply.
    try { Config::instance().load("config.json"); } catch (...) {}
    // Pure CLI mode: populate the demo DB and exit — the user then runs the
    // normal one-shot commands (progressive-cli rooms / view / send / search).
    auto runPureCli = []() {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        // Always refresh the demo data (it clears demo-local rooms first), so
        // re-running `demo populate` picks up any demo changes without having
        // to delete matrixcli.db by hand.
        populateDemoData(dbi);
        std::cout << "Demo data ready. Use the one-shot commands:\n"
                     "  progressive-cli rooms\n"
                     "  progressive-cli view \"#general\" 10\n"
                     "  progressive-cli send \"#general\" \"hello\"\n";
        return 0;
    };
    // The positional shortcuts: demo tui | ui | mobile | cli | markdown | vote
    // — no menu.
    // Run one or more demo targets in order, e.g. `demo edit members typing`
    // executes each showcase sequentially. The takeover modes (tui/ui/mobile/
    // cli) still run only the first one and return, since they grab the
    // terminal. A trailing token after `members` is taken as a room unless it
    // is itself a demo mode.
    if (!args.positional.empty()) {
        auto isDemoMode = [](const std::string& s) -> bool {
            static const char* m[] = {
                "markdown", "md", "vote", "voting", "accounts",
                "tui", "ui", "ascii", "mobile", "phone", "cli", "populate",
                "edit", "report", "members", "typing", "topic", "threads",
                "config", "backup", nullptr};
            for (int i = 0; m[i]; ++i)
                if (s == m[i]) return true;
            return false;
        };
        size_t i = 0;
        bool handled = false;
        while (i < args.positional.size()) {
            const std::string mode = args.positional[i];
            if (!isDemoMode(mode)) break;
            handled = true;

            if (mode == "markdown" || mode == "md") {
                demoMarkdownShowcase();
                ++i; continue;
            }
            if (mode == "accounts") {
                demoAccountsShowcase();
                ++i; continue;
            }
            if (mode == "vote" || mode == "voting") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoVoteShowcase(dbi);
                ++i; continue;
            }
            if (mode == "tui") {
                cli::Args sub = args; sub.options["tui"] = "true";
                // Drop the mode tokens (or the recursion below re-handles
                // them forever and blows the stack).
                sub.positional.erase(sub.positional.begin(),
                                     sub.positional.begin() + (long)(i + 1));
                return cmdDemoRepl(sub);
            }
            if (mode == "ui" || mode == "ascii") {
                cli::Args sub = args; sub.options["ui"] = "true";
                sub.positional.erase(sub.positional.begin(),
                                     sub.positional.begin() + (long)(i + 1));
                return cmdDemoRepl(sub);
            }
            if (mode == "mobile" || mode == "phone") {
                cli::Args sub = args; sub.options["ui"] = "true";
                sub.options["mobile"] = "true";
                sub.positional.erase(sub.positional.begin(),
                                     sub.positional.begin() + (long)(i + 1));
                return cmdDemoRepl(sub);
            }
            if (mode == "cli" || mode == "populate") {
                cli::Args sub = args; sub.options["cli"] = "true";
                sub.positional.erase(sub.positional.begin(),
                                     sub.positional.begin() + (long)(i + 1));
                return cmdDemoRepl(sub);
            }
            if (mode == "edit") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoEditShowcase(dbi);
                ++i; continue;
            }
            if (mode == "report") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoReportShowcase(dbi);
                ++i; continue;
            }
            if (mode == "members") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                std::string rid;
                if (i + 1 < args.positional.size() &&
                    !isDemoMode(args.positional[i + 1])) {
                    rid = matchRoomInCache(dbi.listRooms(),
                                           args.positional[i + 1]);
                    ++i;
                }
                demoMembersShowcase(dbi, rid);
                ++i; continue;
            }
            if (mode == "typing") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoTypingShowcase(dbi);
                ++i; continue;
            }
            if (mode == "topic") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoTopicShowcase(dbi);
                ++i; continue;
            }
            if (mode == "threads") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoThreadsShowcase(dbi);
                ++i; continue;
            }
            if (mode == "config") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoConfigShowcase(dbi);
                ++i; continue;
            }
            if (mode == "backup") {
                db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
                if (dbi.listRooms().empty()) populateDemoData(dbi);
                demoBackupShowcase(dbi);
                ++i; continue;
            }
            ++i;
        }
        if (handled) return 0;
    }

    // One-shot positional form: `demo <room> <action>` (also `demo <action>
    // <room>`), e.g. `matrixcli demo general info`. Runs the action and
    // exits — no interactive REPL and no account needed. Skipped when a
    // takeover mode is active (the room token then belongs to the UI).
    bool takeover = args.options.count("ui") || args.options.count("ascii")
                 || args.options.count("tui") || args.options.count("mobile")
                 || args.options.count("cli");
    if (!takeover) {
        static const char* kOneShot[] = {"info", "view", "rooms",
                                         "search", "power", nullptr};
        auto inSet = [&](const std::string& s) {
            for (int i = 0; kOneShot[i]; ++i)
                if (s == kOneShot[i]) return true;
            return false;
        };
        std::string action, roomArg;
        if (!args.positional.empty() && inSet(args.positional[0])) {
            action = args.positional[0];
            if (args.positional.size() >= 2) roomArg = args.positional[1];
        } else if (args.positional.size() >= 2 && inSet(args.positional[1])) {
            roomArg = args.positional[0];
            action = args.positional[1];
        }
        if (!action.empty()) {
            db::Database dbi;
            if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            cli::Args sub;
            sub.command = action;
            if (!roomArg.empty()) sub.positional.push_back(roomArg);
            for (size_t i = 2; i < args.positional.size(); ++i)
                sub.positional.push_back(args.positional[i]);
            for (auto& kv : args.options) {
                if (kv.first != "cli" && kv.first != "populate")
                    sub.options[kv.first] = kv.second;
            }
            if (action == "info")    return demoReplInfo(dbi, sub);
            if (action == "view") {
                if (!roomArg.empty()) {
                    std::string rid = matchRoomInCache(dbi.listRooms(), roomArg);
                    if (rid.empty() && !roomArg.empty() && roomArg[0] == '#' &&
                        roomArg.find(':') == std::string::npos)
                        rid = matchRoomInCache(dbi.listRooms(), roomArg.substr(1));
                    if (!rid.empty()) sub.positional[0] = rid;
                }
                return cmdView(sub);
            }
            if (action == "rooms")   return cmdRooms(sub);
            if (action == "search")  return cmdSearch(sub);
            if (action == "power")   return cmdPower(sub);
        }
    }

    // A bare `demo <room>` (a single room alias/name/id, no action) showcases
    // that room directly — info + recent messages + power levels — instead of
    // dropping into the interactive REPL. Works for every demo room. Skipped
    // when a takeover mode is active (the room token then belongs to the UI).
    if (!takeover && args.positional.size() == 1) {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (dbi.listRooms().empty()) populateDemoData(dbi);
        const std::string roomArg = args.positional[0];
        std::string roomId = matchRoomInCache(dbi.listRooms(), roomArg);
        if (roomId.empty() && !roomArg.empty() && roomArg[0] == '#' &&
            roomArg.find(':') == std::string::npos)
            roomId = matchRoomInCache(dbi.listRooms(), roomArg.substr(1));
        if (!roomId.empty()) {
            cli::Args sub;
            sub.positional.push_back(roomId);
            sub.options["limit"] = "12";
            std::cout << ANSI_BOLD << "Room showcase: " << ANSI_RESET
                      << roomArg << "\n\n";
            demoReplInfo(dbi, sub);
            std::cout << "\n";
            cmdView(sub);
            std::cout << "\n";
            sub.command = "power";
            cmdPower(sub);
            return 0;
        }
    }

    if (args.options.count("cli") || args.options.count("populate")) {
        return runPureCli();
    }

    // Demo + ASCII interface: 'demo --ui [room]' runs the ASCII client
    // interface; with --static/--once it draws the frame once and exits
    // (non-interactive, pipe-friendly) instead of starting the REPL.
    if (args.options.count("ui") || args.options.count("ascii")) {
        // Tell the user the frame can be drawn once and exited — the
        // pipe-friendly (screenshot) mode.
        if (!args.options.count("static") && !args.options.count("once")
            && isatty(STDOUT_FILENO)) {
            std::cout << "note: demo ui --static draws the frame once and "
                         "exits (non-interactive, pipe-friendly)."
                      << std::endl;
        }
        cli::Args uiArgs;
        // Forward every positional: the first is the room, the rest may be
        // one-shot subcommands like `panel show` / `panel rule …`.
        for (const auto& p : args.positional)
            uiArgs.positional.push_back(p);
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
        if (args.options.count("scroll-center"))
            uiArgs.options["scroll-center"] = args.options.at("scroll-center");
        if (args.options.count("scroll-right"))
            uiArgs.options["scroll-right"] = args.options.at("scroll-right");
        if (args.options.count("scroll-line")) uiArgs.options["scroll-line"] = "true";
        if (args.options.count("scroll-lines"))
            uiArgs.options["scroll-lines"] = args.options.at("scroll-lines");
        if (args.options.count("scroll-page"))
            uiArgs.options["scroll-page"] = args.options.at("scroll-page");
        if (args.options.count("scroll-left")) uiArgs.options["scroll-left"] = args.options.at("scroll-left");
        if (args.options.count("mobile")) uiArgs.options["mobile"] = "true";
        if (args.options.count("panel-left")) uiArgs.options["panel-left"] = args.options.at("panel-left");
        if (args.options.count("panel-right")) uiArgs.options["panel-right"] = args.options.at("panel-right");
        if (args.options.count("panel-auto")) uiArgs.options["panel-auto"] = args.options.at("panel-auto");
        if (args.options.count("members")) uiArgs.options["members"] = args.options.at("members");
        if (args.options.count("space")) uiArgs.options["space"] = args.options.at("space");
        if (args.options.count("time-side")) uiArgs.options["time-side"] = args.options.at("time-side");
        if (args.options.count("msg-line")) uiArgs.options["msg-line"] = args.options.at("msg-line");
        if (args.options.count("room-names")) uiArgs.options["room-names"] = "true";
        if (args.options.count("room-aliases")) uiArgs.options["room-aliases"] = "true";
        if (args.options.count("panel-only"))
            uiArgs.options["panel-only"] = args.options.at("panel-only");
        if (args.options.count("absolute")) uiArgs.options["absolute"] = "true";
        if (args.options.count("msgshr")) uiArgs.options["msgshr"] = "true";
        if (args.options.count("markers")) uiArgs.options["markers"] = "true";
        if (args.options.count("names"))
            uiArgs.options["names"] = args.options.at("names");
        if (args.options.count("mxids"))
            uiArgs.options["mxids"] = args.options.at("mxids");
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
        const char* g = ANSI_BOLD "\033[37m";
        const char* r = ANSI_RESET;
        std::cout << "Choose demo mode:\n"
                     "  1) interactive session (type commands at a prompt)\n"
                     "  2) populate demo data and exit (one-shot commands)\n"
                     "  3) ASCII client interface (rooms | chat | members)\n"
                     "     (non-interactive: progressive-cli demo --ui --static)\n"
                     "  4) terminal UI (ncurses TUI)\n"
                     "  5) ASCII client for smartphones (stacked, portrait)\n"
                     "  6) " << g << "markdown" << r << " rendering demo (demo markdown)\n"
                     "  7) the poll " << g << "vote" << r << " demo (demo vote)\n"
                     "  8) the multi-account demo (demo " << g << "accounts" << r << ")\n"
                     "  9) " << g << "members" << r << " demo (demo members)\n"
                     " 10) " << g << "typing" << r << " demo (demo typing)\n"
                     " 11) " << g << "edit" << r << " demo (demo edit)\n"
                     " 12) " << g << "report" << r << " demo (demo report)\n"
                     " 13) " << g << "topic" << r << " demo (demo topic)\n"
                     " 14) " << g << "threads" << r << " demo (demo threads)\n"
                     " 15) " << g << "config" << r << " demo (demo config)\n"
                     " 16) " << g << "backup" << r << " demo (demo backup)\n"
                     "You can run several in a row, e.g. " << g << "demo edit members typing" << r << "\n"
                     "Choice [1-16]: " << std::flush;
        std::string ans;
        std::getline(std::cin, ans);
        if (!matrixcli::g_interrupted.load()) {
            // Ctrl+C at the choice prompt — leave immediately.
            std::cout << "Bye!" << std::endl;
            return 0;
        }
        int ch = -1;
        if (!ans.empty()) { try { ch = std::stoi(ans); } catch (...) { ch = -1; } }
        if (ch == 2 || ans == "n") {
            return runPureCli();
        }
        if (ch == 3) {
            cli::Args uiArgs;
            return matrixcli::cmdAsciiUi(uiArgs);
        }
        if (ch == 5) {
            cli::Args uiArgs;
            uiArgs.options["mobile"] = "true";
            return matrixcli::cmdAsciiUi(uiArgs);
        }
        if (ch == 6) {
            demoMarkdownShowcase();
            return 0;
        }
        if (ch == 7) {
            db::Database dbi;
            if (!dbi.open("matrixcli.db")) return 1;
            demoVoteShowcase(dbi);
            return 0;
        }
        if (ch == 8) {
            demoAccountsShowcase();
            return 0;
        }
        if (ch == 9) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoMembersShowcase(dbi, "");
        }
        if (ch == 10) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoTypingShowcase(dbi);
        }
        if (ch == 11) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoEditShowcase(dbi);
        }
        if (ch == 12) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoReportShowcase(dbi);
        }
        if (ch == 13) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoTopicShowcase(dbi);
        }
        if (ch == 14) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoThreadsShowcase(dbi);
        }
        if (ch == 15) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoConfigShowcase(dbi);
        }
        if (ch == 16) {
            db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            return demoBackupShowcase(dbi);
        }
        if (ch == 4) {
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

    const char* g = ANSI_BOLD "\033[37m";
    const char* r = ANSI_RESET;
    std::cout << "progressive-cli demo — interactive mode (offline, no account needed)" << std::endl;
    std::cout << "Commands: help | rooms | view <room> [n] | info <room> | power <room> |"
              << " search <query> | send <room> <text> | attach <room> <file> |\n"
              << "          ui | clear | quit | " << g << "markdown" << r << " | " << g << "vote" << r << " | " << g << "accounts" << r
              << " | " << g << "edit" << r << " | " << g << "report" << r << " | " << g << "members" << r
              << " | " << g << "typing" << r << " | " << g << "topic" << r << " | " << g << "threads" << r
              << " | " << g << "config" << r << " | " << g << "backup" << r << "\n"
              << std::endl;
    std::cout << "  (white = demo-dedicated).  Demo rooms: " << g << "#general" << r
              << "  " << g << "#dev" << r << "  " << g << "#random" << r
              << "  " << g << "#dm_alice" << r << "  " << g << "#dm_bob" << r
              << std::endl;

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
            const char* g = ANSI_BOLD "\033[37m";
            const char* r = ANSI_RESET;
            std::cout << "Regular commands (run against the offline demo data):\n"
                         "  rooms                     list demo rooms\n"
                         "  view <room> [n]           show the last n messages (default 20)\n"
                         "  info <room>               show room info (id, topic, members, E2EE…)\n"
                         "  power <room>              show room power levels / permissions\n"
                         "  search <query>            full-text search in cached messages\n"
                         "  send <room> <text>        send a message (demo, offline)\n"
                         "  attach <room> <file>      attach a file (demo, offline)\n"
                         "  ui                        ASCII client interface (rooms | chat | members)\n"
                         "  clear | quit / exit       leave the demo\n"
                         "\n"
                           "Dedicated feature demos " << g << "(white = demo-dedicated)" << r
                          << " — showcase one feature in isolation:\n"
                           "  " << g << "markdown" << r << "                   the markdown rendering demo\n"
                           "  " << g << "vote" << r << "                       pick a voting and vote\n"
                           "  " << g << "accounts" << r << "                   the multi-account demo\n"
                           "  " << g << "demo <room>" << r << "              showcase any demo room (info + messages + power)\n"
                           "  " << g << "members" << r << "                   list room members (scroll + pick another room)\n"
                           "  " << g << "typing" << r << "                    show typing indicators\n"
                           "  " << g << "edit" << r << "                      edit a message (pick room + message)\n"
                           "  " << g << "report" << r << "                    report a message to the admin\n"
                           "  " << g << "topic" << r << "                     set a room topic\n"
                           "  " << g << "threads" << r << "                  view a message's thread\n"
                           "  " << g << "config" << r << "                   show/edit the client config\n"
                           "  " << g << "backup" << r << "                   key backup create/upload/restore/delete\n";
            continue;
        }
        if (a.command == "clear") {
            std::cout << "\033[2J\033[H" << std::flush;
            continue;
        }
        if (a.command == "rooms") { cmdRooms(a); continue; }
        if (a.command == "view") { cmdView(a); continue; }
        if (a.command == "info") { demoReplInfo(dbi, a); continue; }
        if (a.command == "power" || a.command == "perms") { cmdPower(a); continue; }
        if (a.command == "search") { cmdSearch(a); continue; }
        if (a.command == "send") { demoReplSend(a); continue; }
        if (a.command == "markdown" || a.command == "md") {
            demoMarkdownShowcase();
            continue;
        }
        if (a.command == "vote" || a.command == "voting") {
            demoVoteShowcase(dbi);
            continue;
        }
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
        if (a.command == "members") {
            std::string rid;
            if (a.positional.size() >= 1)
                rid = matchRoomInCache(dbi.listRooms(), a.positional[0]);
            demoMembersShowcase(dbi, rid);
            continue;
        }
        if (a.command == "typing") { demoTypingShowcase(dbi); continue; }
        if (a.command == "edit") { demoEditShowcase(dbi); continue; }
        if (a.command == "report") { demoReportShowcase(dbi); continue; }
        if (a.command == "topic") { demoTopicShowcase(dbi); continue; }
        if (a.command == "threads") { demoThreadsShowcase(dbi); continue; }
        if (a.command == "config") { demoConfigShowcase(dbi); continue; }
        if (a.command == "backup") { demoBackupShowcase(dbi); continue; }
        std::cout << "Unknown command '" << a.command << "' — type 'help'." << std::endl;
    }
    std::cout << "Bye!" << std::endl;
    return 0;
}

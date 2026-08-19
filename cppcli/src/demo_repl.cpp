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

// The markdown rendering demo: the same sample that also lives in the
// #general demo room, rendered by the same ANSI renderer the chat view uses
// (renderMarkdownBody in ascii_ui_helpers.cpp).
void demoMarkdownShowcase() {
    static const char* kSample =
        "Markdown demo: **bold text**, *italic*, `inline code`, "
        "[a link](https://matrix.org) and a raw https://matrix.org\n"
        "\n"
        "# A header\n"
        "\n"
        "- a bullet\n"
        "- another bullet\n"
        "- [x] done task\n"
        "- [ ] open task\n"
        "\n"
        "1. first step\n"
        "2. second step\n"
        "\n"
        "> a quoted line\n"
        "\n"
        "```cpp\n"
        "#include <iostream>\n"
        "\n"
        "int main() {\n"
        "    // a highlighted comment\n"
        "    const char* s = \"hello markdown\";\n"
        "    std::cout << s << std::endl;\n"
        "    return 0;\n"
        "}\n"
        "```";
    std::cout << "Markdown rendering demo — the ANSI renderer the chat view uses.\n"
                 "The same message lives in #general (demo): run view \"#general\""
              << std::endl;
    if (isatty(STDOUT_FILENO)) {
        std::cout << renderMarkdownBody(kSample) << std::endl;
    } else {
        std::cout << "(stdout is not a terminal — the styled output only shows "
                     "interactively; here is the plain source:)" << std::endl;
        std::cout << kSample << std::endl;
    }
}

// The poll vote demo: scans the cache (demo OR real data — same database
// code path) for m.poll.start events, lets the user pick a voting, shows
// the question and the current tallies, and records their vote as an
// m.poll.response event (the wire format the UI renders).
void demoVoteShowcase(db::Database& dbi) {
    struct PollInfo {
        std::string roomId, roomName, id, question;
        std::vector<std::pair<std::string, std::string>> answers;
    };
    std::vector<PollInfo> polls;
    std::set<std::string> roomsWith;
    for (const auto& r : dbi.listRooms()) {
        std::string roomId = r.value("room_id", "");
        if (roomId.empty()) continue;
        std::string roomName = r.value("name", "");
        if (roomName.empty()) roomName = roomId;
        auto evs = dbi.getEvents(roomId, 5000);
        for (const auto& ev : evs) {
            if (!ev.content.is_object()) continue;
            if (ev.content.value("msgtype", "") != "m.poll.start") continue;
            PollInfo pi;
            pi.roomId = roomId;
            pi.roomName = roomName;
            pi.id = ev.event_id;
            auto q = ev.content.find("question");
            if (q != ev.content.end() && q->is_object()) {
                pi.question = q->value("text", "");
            }
            auto an = ev.content.find("answers");
            if (an != ev.content.end() && an->is_array()) {
                for (const auto& a : *an) {
                    if (a.is_object()) {
                        pi.answers.emplace_back(a.value("id", ""),
                                                a.value("text", ""));
                    }
                }
            }
            if (pi.question.empty()) pi.question = pi.id;
            polls.push_back(std::move(pi));
            roomsWith.insert(roomId);
        }
    }
    if (polls.empty()) {
        std::cout << "No votings in the cache yet (run 'demo populate' first)."
                  << std::endl;
        return;
    }
    std::cout << "The client knows about " << polls.size() << " votings in "
              << roomsWith.size() << " rooms:" << std::endl;
    for (size_t i = 0; i < polls.size(); ++i) {
        std::cout << "  " << (i + 1) << ": [" << polls[i].roomName << "] "
                  << polls[i].question << "  (" << polls[i].answers.size()
                  << " answers)" << std::endl;
    }
    if (!isatty(STDIN_FILENO)) {
        std::cout << "(run it on a terminal to vote — or 'view <room>' shows "
                     "the tallies in the chat)" << std::endl;
        return;
    }
    std::cout << "Which voting are you in? [1-" << polls.size() << "]: "
              << std::flush;
    std::string pick;
    std::getline(std::cin, pick);
    long idx = -1;
    try {
        long v = std::stol(pick);
        if (v >= 1 && v <= static_cast<long>(polls.size())) idx = v - 1;
    } catch (...) {}
    if (idx < 0) {
        std::cout << "No such voting." << std::endl;
        return;
    }
    const PollInfo& p = polls[idx];
    std::cout << "Voting #" << (idx + 1) << ": " << p.question << "  ("
              << p.roomName << ")" << std::endl;
    // The current tallies (the m.poll.response events for this poll).
    std::map<std::string, int> tally;
    for (const auto& ev : dbi.getEvents(p.roomId, 5000)) {
        if (!ev.content.is_object()) continue;
        if (ev.content.value("msgtype", "") != "m.poll.response") continue;
        auto rel = ev.content.find("m.relates_to");
        if (rel == ev.content.end() || !rel->is_object()) continue;
        if (rel->value("event_id", "") != p.id) continue;
        auto sel = ev.content.find("selections");
        if (sel != ev.content.end() && sel->is_array() && !sel->empty() &&
            (*sel)[0].is_string()) {
            tally[(*sel)[0].get<std::string>()]++;
        }
    }
    for (size_t i = 0; i < p.answers.size(); ++i) {
        std::cout << "  " << (i + 1) << ": " << p.answers[i].second
                  << "  (" << tally[p.answers[i].first] << " votes)"
                  << std::endl;
    }
    std::cout << "Vote for? [1-" << p.answers.size() << "]: " << std::flush;
    std::string ans;
    std::getline(std::cin, ans);
    long aidx = -1;
    try {
        long v = std::stol(ans);
        if (v >= 1 && v <= static_cast<long>(p.answers.size())) aidx = v - 1;
    } catch (...) {}
    if (aidx < 0) {
        std::cout << "No such answer." << std::endl;
        return;
    }
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    matrix::Event r;
    r.event_id = "$demo_vote_you_" + std::to_string(ts);
    r.room_id = p.roomId;
    r.sender = "@you";
    r.type = "m.room.message";
    r.content = {{"msgtype", "m.poll.response"},
                 {"m.relates_to",
                  {{"event_id", p.id}, {"rel_type", "m.reference"}}},
                 {"selections", {p.answers[aidx].first}}};
    r.origin_server_ts = ts;
    dbi.insertEvent(r);
    std::cout << "Voted for \"" << p.answers[aidx].second << "\" in "
              << p.roomName << " (recorded locally)." << std::endl;
    std::cout << "See it in the chat: view \"" << p.roomName
              << "\" 30 — or run 'ui' and open the room." << std::endl;
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
    // The positional shortcuts: demo tui | ui | mobile | cli | markdown | vote
    // — no menu.
    if (!args.positional.empty()) {
        const std::string mode = args.positional[0];
        cli::Args sub = args;
        sub.positional.erase(sub.positional.begin());
        if (mode == "markdown" || mode == "md") {
            demoMarkdownShowcase();
            return 0;
        }
        if (mode == "vote" || mode == "voting") {
            db::Database dbi;
            if (!dbi.open("matrixcli.db")) return 1;
            if (dbi.listRooms().empty()) populateDemoData(dbi);
            demoVoteShowcase(dbi);
            return 0;
        }
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
        // Tell the user the frame can be drawn once and exited — the
        // pipe-friendly (screenshot) mode.
        if (!args.options.count("static") && !args.options.count("once")
            && isatty(STDOUT_FILENO)) {
            std::cout << "note: demo ui --static draws the frame once and "
                         "exits (non-interactive, pipe-friendly)."
                      << std::endl;
        }
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
        std::cout << "Choose demo mode:\n"
                     "  1) interactive session (type commands at a prompt)\n"
                     "  2) populate demo data and exit (one-shot commands)\n"
                     "  3) ASCII client interface (rooms | chat | members)\n"
                     "     (non-interactive: progressive-cli demo --ui --static)\n"
                     "  4) terminal UI (ncurses TUI)\n"
                     "  5) ASCII client for smartphones (stacked, portrait)\n"
                     "  6) markdown rendering demo (demo markdown)\n"
                     "  7) the poll vote demo (demo vote)\n"
                     "Choice [1/2/3/4/5/6/7]: " << std::flush;
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
        if (!ans.empty() && ans[0] == '6') {
            demoMarkdownShowcase();
            return 0;
        }
        if (!ans.empty() && ans[0] == '7') {
            db::Database dbi;
            if (!dbi.open("matrixcli.db")) return 1;
            demoVoteShowcase(dbi);
            return 0;
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
              << " send <room> <text> | markdown | vote | ui | clear | quit" << std::endl;
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
                         "  markdown                  show the markdown rendering demo\n"
                         "  vote                      pick a voting (of the N the cache knows) and vote\n"
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
        std::cout << "Unknown command '" << a.command << "' — type 'help'." << std::endl;
    }
    std::cout << "Bye!" << std::endl;
    return 0;
}

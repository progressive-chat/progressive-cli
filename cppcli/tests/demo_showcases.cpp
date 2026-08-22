// demo_showcases.cpp — offline demo showcase helpers + flows, split out of
// demo_repl.cpp (which kept the REPL, the menu and the one-shot dispatch).
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
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

using namespace matrixcli;

#include "demo_showcases.hpp"

std::string demoShortSender(const std::string& s) {
    std::string x = s;
    if (!x.empty() && x[0] == '@') {
        auto at = x.find(':');
        if (at != std::string::npos) x = x.substr(1, at - 1);
    }
    return x;
}

std::string demoFormatTs(int64_t ts) {
    if (ts <= 0) return "";
    std::time_t t = (std::time_t)(ts / 1000);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return std::string(buf);
}

std::string demoRoomDisplayName(db::Database& dbi, const std::string& roomId) {
    for (auto& r : dbi.listRooms())
        if (r.value("room_id", "") == roomId) {
            std::string a = r.value("canonical_alias", "");
            if (!a.empty()) return a;
            return r.value("name", roomId);
        }
    return roomId;
}

// Interactive room picker: lists demo rooms (optionally biggest-first),
// accepts a number, an alias/name, or a substring. Empty string = cancel.
std::string demoPickRoom(db::Database& dbi, bool sortByMembers,
                                const char* purpose) {
    auto all = dbi.listRooms();
    if (sortByMembers)
        std::sort(all.begin(), all.end(),
                  [](const nlohmann::json& a, const nlohmann::json& b) {
                      return a.value("member_count", 0) > b.value("member_count", 0);
                  });
    const size_t cap = 40;
    size_t shown = std::min(all.size(), cap);
    std::cout << "Select a room";
    if (purpose) std::cout << " (" << purpose << ")";
    std::cout << " — " << all.size() << " demo rooms:\n";
    for (size_t i = 0; i < shown; ++i) {
        const auto& r = all[i];
        std::string alias = r.value("canonical_alias", "");
        std::string name = r.value("name", "");
        int mc = r.value("member_count", 0);
        std::string label = alias.empty() ? name : (alias + "  (" + name + ")");
        std::cout << "  " << (i + 1) << ") " << label << "  — " << mc << " members\n";
    }
    if (all.size() > cap)
        std::cout << "  … showing " << cap << " of " << all.size()
                  << " rooms. Type a name/alias to filter, or 'rooms' to list all.\n";
    std::cout << "Room # or alias (or 'q' to cancel): " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return "";
    auto b = line.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    auto e = line.find_last_not_of(" \t");
    line = line.substr(b, e - b + 1);
    if (line == "q" || line == "quit" || line == "exit") return "";
    try {
        size_t idx = std::stoul(line);
        if (idx >= 1 && idx <= all.size()) return all[idx - 1].value("room_id", "");
    } catch (...) {}
    std::string rid = matchRoomInCache(all, line);
    if (rid.empty() && !line.empty() && line[0] == '#')
        rid = matchRoomInCache(all, line.substr(1));
    if (rid.empty()) {
        for (auto& r : all) {
            std::string a = r.value("canonical_alias", "");
            std::string n = r.value("name", "");
            if ((!a.empty() && a.find(line) != std::string::npos) ||
                (!n.empty() && n.find(line) != std::string::npos))
                return r.value("room_id", "");
        }
    }
    return rid;
}

// A short human-readable label for any event, used by the message picker so
// that state events (joins, power levels, …) show their type instead of an
// empty body.
static std::string demoEventLabel(const matrix::Event& ev) {
    if (ev.type == "m.room.message") {
        std::string b = ev.content.value("body", "");
        if (!b.empty()) return b;
        return ev.content.value("msgtype", ev.type);
    }
    std::string t = ev.type;
    if (t.rfind("m.", 0) == 0) t = t.substr(2);
    if (ev.type == "m.room.member")
        return "[" + t + "] " + ev.content.value("membership", "?");
    if (ev.type == "m.room.name")
        return "[" + t + "] " + ev.content.value("name", "");
    if (ev.type == "m.room.topic")
        return "[" + t + "] " + ev.content.value("topic", "");
    if (ev.type == "m.room.canonical_alias")
        return "[" + t + "] " + ev.content.value("alias", "");
    return "[" + t + "]";
}

// Pick a message/event in a room: list recent events, accept a number or a
// substring search. Returns an empty event on cancel / no events.
matrix::Event demoPickMessage(db::Database& dbi, const std::string& roomId) {
    auto evs = dbi.getEvents(roomId, 500);
    if (evs.empty()) {
        std::cout << "No events in this room in the demo data.\n";
        return matrix::Event{};
    }
    for (;;) {
        size_t cap = 20;
        size_t start = evs.size() > cap ? evs.size() - cap : 0;
        std::cout << "Select an event (" << evs.size() << " total; showing last "
                  << (evs.size() - start) << "):\n";
        for (size_t i = start; i < evs.size(); ++i) {
            const auto& ev = evs[i];
            std::string body = demoEventLabel(ev);
            if (body.size() > 60) body = body.substr(0, 57) + "...";
            std::cout << "  " << (i + 1) << ") [" << demoFormatTs(ev.origin_server_ts)
                      << "] " << demoShortSender(ev.sender) << ": " << body << "\n";
        }
        std::cout << "Event #, or text to search (or 'q'): " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return matrix::Event{};
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line == "q" || line == "quit" || line == "exit") return matrix::Event{};
        try {
            size_t idx = std::stoul(line);
            if (idx >= 1 && idx <= evs.size()) return evs[idx - 1];
        } catch (...) {}
        std::string ql = line;
        std::transform(ql.begin(), ql.end(), ql.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (auto& ev : evs) {
            std::string bdy = demoEventLabel(ev);
            std::transform(bdy.begin(), bdy.end(), bdy.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (bdy.find(ql) != std::string::npos) return ev;
        }
        std::cout << "No event matches '" << line << "'. Try again.\n";
    }
}

// The demo DB stores only member_count, so we synthesize a stable roster.
std::vector<std::tuple<std::string, std::string, std::string>>
demoRoomMembers(const std::string& roomId, int count) {
    std::vector<std::tuple<std::string, std::string, std::string>> out;
    out.push_back({"Alice", "@alice:demo.local", "admin"});
    out.push_back({"Bob", "@bob:demo.local", "moderator"});
    out.push_back({"Carol", "@carol:demo.local", "member"});
    out.push_back({"Dave", "@dave:demo.local", "member"});
    const char* pool[] = {"erin", "frank", "grace", "heidi", "ivan", "judy",
                          "mallory", "nol", "oscar", "peggy", "trent", "victor",
                          "walter", "xavier", "yvonne", "zoe", "quinn", "rita"};
    unsigned seed = 0;
    for (char c : roomId) seed = seed * 31u + (unsigned char)c;
    size_t n = sizeof(pool) / sizeof(pool[0]);
    for (int i = (int)out.size(); i < count && i < 400; ++i) {
        const char* base = pool[(seed + i) % n];
        std::string num = std::to_string(1000 + ((seed + i * 7) % 9000));
        std::string uid = "@" + std::string(base) + num + ":demo.local";
        std::string disp = std::string(base);
        disp[0] = (char)std::toupper((unsigned char)disp[0]);
        out.push_back({disp, uid, "member"});
    }
    return out;
}

int demoMembersShowcase(db::Database& dbi, const std::string& roomArg) {
    auto all = dbi.listRooms();
    std::string roomId = roomArg;
    if (roomId.empty()) {
        int best = -1;
        for (auto& r : all) {
            int mc = r.value("member_count", 0);
            if (mc > best) { best = mc; roomId = r.value("room_id", ""); }
        }
    }
    if (roomId.empty()) { std::cout << "No demo rooms.\n"; return 1; }
    int mc = 0;
    for (auto& r : all)
        if (r.value("room_id", "") == roomId) { mc = r.value("member_count", 0); break; }
    auto members = demoRoomMembers(roomId, mc > 0 ? mc : 20);
    std::string name = demoRoomDisplayName(dbi, roomId);
    size_t shown = std::min(members.size(), (size_t)20);
    std::cout << "\nMembers of " << name << " (" << members.size() << " members):\n";
    for (size_t i = 0; i < shown; ++i) {
        const auto& [disp, uid, role] = members[i];
        std::string tag = role == "admin" ? "  [admin]" : role == "moderator" ? "  [mod]" : "";
        std::cout << "  " << (i + 1) << ") " << disp << "  " << uid << tag << "\n";
    }
    std::cout << "  … showing " << shown << " of " << members.size() << ".\n";
    std::cout << "  Scroll the member list in the live UI with PageDown/PageUp or Up/Down; "
                 "type to jump to a name.\n";
    std::vector<nlohmann::json> sorted = all;
    std::sort(sorted.begin(), sorted.end(),
               [](const nlohmann::json& a, const nlohmann::json& b) {
                   return a.value("member_count", 0) > b.value("member_count", 0);
               });
    std::cout << "  Pick another room — the biggest rooms:\n";
    for (size_t i = 0; i < std::min(sorted.size(), (size_t)12); ++i) {
        const auto& r = sorted[i];
        std::string a = r.value("canonical_alias", "");
        std::string label = a.empty() ? r.value("name", "") : a;
        std::cout << "    " << label << " (" << r.value("member_count", 0) << ")\n";
    }
    std::cout << "  Run: progressive-cli demo members <alias>\n";
    return 0;
}

int demoTypingShowcase(db::Database& dbi) {
    auto all = dbi.listRooms();
    const char* sample[] = {"#dev", "#general", "#random", "#music"};
    std::cout << "\nTyping indicators (demo snapshot — the offline demo has no live typers):\n";
    bool any = false;
    for (const char* alias : sample) {
        std::string rid = matchRoomInCache(all, alias);
        if (rid.empty()) continue;
        std::string who;
        if (alias == std::string("#dev")) who = "@bob:demo.local";
        else if (alias == std::string("#random")) who = "@carol:demo.local";
        if (!who.empty()) {
            any = true;
            std::cout << "  " << alias << " — " << demoShortSender(who) << " is typing…\n";
        } else {
            std::cout << "  " << alias << " — nobody is typing\n";
        }
    }
    if (!any) std::cout << "  Nobody is typing now.\n";
    std::cout << "  Live: progressive-cli typing <room> shows who is currently typing.\n";
    return 0;
}

int demoEditShowcase(db::Database& dbi) {
    if (!isatty(STDIN_FILENO)) {
        std::cout << "[demo] edit: pick a room, pick a message, then edit it.\n"
                  << "[demo] Run in a terminal for the interactive flow.\n";
        return 0;
    }
    std::string roomId;
    for (;;) {
        roomId = demoPickRoom(dbi, false, "to edit a message in");
        if (roomId.empty()) { std::cout << "Cancelled.\n"; return 0; }
        if (!dbi.getEvents(roomId, 1).empty()) break;
        std::cout << "That room has no messages in the demo — pick another.\n";
    }
    matrix::Event ev = demoPickMessage(dbi, roomId);
    if (ev.event_id.empty()) { std::cout << "Cancelled.\n"; return 0; }
    std::string name = demoRoomDisplayName(dbi, roomId);
    std::string original = ev.content.value("body", "");
    std::cout << "\n+------------------------------------------------+\n"
              << "| Edit message in " << name << "\n"
              << "| From: " << demoShortSender(ev.sender)
              << "   " << demoFormatTs(ev.origin_server_ts) << "\n"
              << "| Original:\n";
    { std::istringstream iss(original); std::string l;
      while (std::getline(iss, l)) std::cout << "|   " << l << "\n"; }
    std::cout << "| New text (Enter to keep, or type replacement): " << std::flush;
    std::string nl;
    if (!std::getline(std::cin, nl)) nl = "";
    auto b = nl.find_first_not_of(" \t");
    std::string updated = (b == std::string::npos) ? original : nl;
    std::cout << "| Result (sends m.room.message with m.relates_to m.replace\n"
              << "|         -> edits in place for everyone):\n"
              << "|   * " << updated << "\n"
              << "+------------------------------------------------+\n";
    std::cout << "[demo] Live: progressive-cli edit " << name << " " << ev.event_id
              << " <text>\n";
    return 0;
}

int demoReportShowcase(db::Database& dbi) {
    if (!isatty(STDIN_FILENO)) {
        std::cout << "[demo] report: pick a room, pick a message, then report it.\n"
                  << "[demo] Run in a terminal for the interactive flow.\n";
        return 0;
    }
    std::string roomId;
    for (;;) {
        roomId = demoPickRoom(dbi, false, "to report a message in");
        if (roomId.empty()) { std::cout << "Cancelled.\n"; return 0; }
        if (!dbi.getEvents(roomId, 1).empty()) break;
        std::cout << "That room has no messages in the demo — pick another.\n";
    }
    matrix::Event ev = demoPickMessage(dbi, roomId);
    if (ev.event_id.empty()) { std::cout << "Cancelled.\n"; return 0; }
    std::string name = demoRoomDisplayName(dbi, roomId);
    std::string body = ev.content.value("body", "");
    if (body.size() > 70) body = body.substr(0, 67) + "...";
    std::cout << "\n+------------------------------------------------+\n"
              << "| Report a message from " << demoShortSender(ev.sender)
              << " in " << name << ":\n"
              << "|   \"" << body << "\"\n"
              << "| Choose a reason:\n"
              << "|   1) spam / unwanted promotion\n"
              << "|   2) abusive or harassing\n"
              << "|   3) offensive content\n"
              << "|   4) misleading or fraudulent\n"
              << "|   5) other\n"
              << "| Reason [1-5] (or 'q'): " << std::flush;
    std::string choice;
    if (!std::getline(std::cin, choice)) choice = "q";
    if (choice == "q" || choice == "quit") { std::cout << "Cancelled.\n"; return 0; }
    const char* reasons[] = {"", "spam", "abusive", "offensive", "misleading", "other"};
    int ri = 0; try { ri = std::stoi(choice); } catch (...) {}
    std::string reason = (ri >= 1 && ri <= 5) ? reasons[ri] : "other";
    std::cout << "| Note (optional): " << std::flush;
    std::string note; std::getline(std::cin, note);
    std::cout << "| -> Reported " << ev.event_id << " to the homeserver admin (reason: "
              << reason << (note.empty() ? "" : ", note: " + note) << ")\n"
              << "+------------------------------------------------+\n";
    std::cout << "[demo] Live: progressive-cli report " << name << " " << ev.event_id
              << " --reason " << reason << "\n";
    return 0;
}

int demoTopicShowcase(db::Database& dbi) {
    if (!isatty(STDIN_FILENO)) {
        std::cout << "[demo] topic: pick a room, then set its topic. Run in a terminal.\n";
        return 0;
    }
    std::string roomId = demoPickRoom(dbi, false, "to edit the topic of");
    if (roomId.empty()) { std::cout << "Cancelled.\n"; return 0; }
    std::string name = demoRoomDisplayName(dbi, roomId);
    std::string cur;
    for (auto& r : dbi.listRooms())
        if (r.value("room_id", "") == roomId) { cur = r.value("topic", ""); break; }
    std::cout << "\n+------------------------------------------------+\n"
              << "| Topic of " << name << "\n"
              << "| Current:\n|   " << (cur.empty() ? "(no topic)" : cur) << "\n"
              << "| New topic (Enter to keep): " << std::flush;
    std::string nl;
    if (!std::getline(std::cin, nl)) nl = "";
    auto b = nl.find_first_not_of(" \t");
    std::string updated = (b == std::string::npos) ? cur : nl;
    std::cout << "| Result (sends m.room.topic state event):\n|   " << updated << "\n"
              << "+------------------------------------------------+\n";
    std::cout << "[demo] Live: progressive-cli topic " << name << " <text>\n";
    return 0;
}

int demoThreadsShowcase(db::Database& dbi) {
    if (!isatty(STDIN_FILENO)) {
        std::cout << "[demo] threads: pick a room, pick a root message, then view its thread.\n"
                  << "[demo] Run in a terminal for the interactive flow.\n";
        return 0;
    }
    std::string roomId;
    for (;;) {
        roomId = demoPickRoom(dbi, false, "to view a thread in");
        if (roomId.empty()) { std::cout << "Cancelled.\n"; return 0; }
        if (!dbi.getEvents(roomId, 1).empty()) break;
        std::cout << "That room has no messages in the demo — pick another.\n";
    }
    matrix::Event root = demoPickMessage(dbi, roomId);
    if (root.event_id.empty()) { std::cout << "Cancelled.\n"; return 0; }
    auto evs = dbi.getEvents(roomId, 2000);
    std::vector<matrix::Event> replies;
    for (auto& ev : evs) {
        auto rel = ev.content.find("m.relates_to");
        if (rel != ev.content.end() && rel->is_object() &&
            rel->value("rel_type", "") == "m.thread" &&
            rel->value("event_id", "") == root.event_id)
            replies.push_back(ev);
    }
    std::string name = demoRoomDisplayName(dbi, roomId);
    std::cout << "\n+------------------------------------------------+\n"
              << "| Thread in " << name << " (root by " << demoShortSender(root.sender) << "):\n";
    std::string rb = root.content.value("body", "");
    if (rb.size() > 70) rb = rb.substr(0, 67) + "...";
    std::cout << "|   " << rb << "\n";
    if (replies.empty()) std::cout << "|   (no replies in this thread in the demo data)\n";
    else for (auto& rp : replies) {
        std::string b = rp.content.value("body", "");
        if (b.size() > 70) b = b.substr(0, 67) + "...";
        std::cout << "|   `- " << demoShortSender(rp.sender) << ": " << b << "\n";
    }
    std::cout << "+------------------------------------------------+\n";
    std::cout << "[demo] Live: progressive-cli threads " << name << " (opens the thread timeline)\n";
    return 0;
}

int demoConfigShowcase(db::Database& dbi) {
    (void)dbi;
    const std::string path = "matrixcli.toml";
    std::cout << "Client config (" << path << "):\n";
#ifdef BUILD_TUI
    tui::TUIConfig cfg = tui::TUIConfig::load(path);
    std::cout << "show_timestamps = " << cfg.show_timestamps << "\n";
    std::cout << "compact         = " << cfg.compact_mode << "\n";
    std::cout << "sound           = " << cfg.notification_sound << "\n";
    std::cout << "room_width      = " << cfg.room_list_width << "\n";
    std::cout << "max_messages    = " << cfg.max_messages << "\n";
    std::cout << "date_format     = " << cfg.date_format << "\n";
#else
    std::cout << "show_timestamps = true\n";
    std::cout << "compact         = false\n";
    std::cout << "sound           = false\n";
    std::cout << "room_width      = 28\n";
    std::cout << "max_messages    = 200\n";
    std::cout << "date_format     = %H:%M\n";
#endif
    std::cout << "\nSet:  progressive-cli config --set key value\n";
    return 0;
}

int demoBackupShowcase(db::Database& dbi) {
    (void)dbi;
    std::cout << "Key backup (demo) — recover your encryption keys if you lose a device.\n";
    std::cout << "Sub-actions:\n";
    std::cout << "  create    Generate a recovery key (shown ONCE)\n";
    std::cout << "  upload    Upload locally cached keys to the backup\n";
    std::cout << "  restore   Re-import keys with --recovery-key <key>\n";
    std::cout << "  delete    Remove the server-side backup\n";
    std::string key = "AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GGGG-HHHH";
    std::cout << "\n[demo] create -> Key backup created. SAVE THIS RECOVERY KEY (shown once):\n\n"
              << "  " << key << "\n\n";
    std::cout << "[demo] Live: progressive-cli backup create | upload | restore --recovery-key <key> | delete\n";
    return 0;
}

// ---- Interactive demo REPL (offline, no Matrix account needed) ----
//
// Replaces the old `demo` behavior (which started an HTTP API server on
// port 8080). Now `progressive-cli demo` drops the user into an interactive
// terminal session against the offline demo database: type commands, see
// output, same handlers as the real CLI (rooms/view/search). The web demo
// stays available as `progressive-cli serve --demo`.

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


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

int asciiReplDispatchA(UiState& st, db::Database& dbi, const cli::Args& a) {
        if (a.command == "quit" || a.command == "exit") return 2;
        if (a.command == "help") {
            std::cout << "  the chat:\n"
                         "  open <room> / view [room]  open a room\n"
                         "  send <room> <text>   send a message\n"
                         "  find <query> / search <query>  the message search (bodies, mxids, links)\n"
                         "  thread <room> [root]  the threads\n"
                         "  up / down [n] / top / bottom / scroll <n>  the viewport\n"
                         "  goto <event_id> | lastread | successor | newest   jump to a place\n"
                         "  pins  the pinned messages / pin <HH:MM> <[nick]>  pin one\n"
                         "  rooms / refresh    the cache + the redraw\n"
                         "\n"
                         "  the rooms + the members:\n"
                         "  mute <room> on|off  the indicators\n"
                         "  invite <room> <@user>  invite someone\n"
                         "  powerlevels  the full power levels (incl. the custom ones)\n"
                         "  nick <room> <@user> <name>  the per-room names\n"
                         "\n"
                         "  the settings (settings [keyword] filters them):\n"
                         "  settings / receipts show|send on|off / sendtyping on|off\n"
                         "  modredact on|off / invreason chat|menu on|off\n"
                         "  threads on|off / sendpreset original|compact|full\n"
                         "  nickname <name> / avatar <url> / presence online|away|offline\n"
                         "  mobile on|off / time / clock / names / emoji ...\n"
                         "\n"
                         "  the misc:\n"
                         "  dump <room> [--format json|txt|html|md]  export a room\n"
                         "  account <@user> / accounts / spaces / space <name>\n"
                         "  agent ...  the coding agent / llm ... the LLM chat\n"
                         "  help / quit / exit\n";
            return 1;
        }
        if (asciiSettingsCommand(st, dbi, a)) return 1;
        if (a.command == "open" || a.command == "view") {
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            loadRoomIntoStateImpl(st, q);
            if (st.mobile) {
                st.mobileTab = 1;  // Element Classic: opening jumps to Chat
                st.scroll = 0;
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "thread") {
            if (!a.positional.empty() && a.positional[0] == "off") {
                st.rightPanel = 0;
                st.statusNote = "right panel: members";
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            auto isNum = [](const std::string& x) {
                return !x.empty() &&
                    std::all_of(x.begin(), x.end(),
                                [](unsigned char c) { return std::isdigit(c); });
            };
            // "thread 2" alone: the N-th thread of the CURRENT room.
            bool singleNum = a.positional.size() == 1 && isNum(a.positional[0]);
            if (a.positional.size() < 2 && !singleNum) {
                // The room's thread LIST in the right panel.
                std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
                std::string tRoom = q;
                for (const auto& r : st.rooms) {
                    std::string id = r.value("room_id", "");
                    std::string name = r.value("name", "");
                    if (id == q || id.find(q) != std::string::npos ||
                        name == q || name.find(q) != std::string::npos) {
                        tRoom = id;
                        break;
                    }
                }
                st.rightPanel = 1;
                st.threadRoomId = tRoom;
                st.statusNote = "right panel: threads of " + tRoom
                              + " — thread <room> <N|id> opens one";
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            // One thread in the right panel: root + replies. The thread is
            // picked by its NUMBER in the list (thread 2, thread <room> 3)
            // or by the root id / a substring of it.
            std::string q = singleNum ? st.currentRoomId : a.positional[0];
            std::string tRoom = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == q || id.find(q) != std::string::npos ||
                    name == q || name.find(q) != std::string::npos) {
                    tRoom = id;
                    break;
                }
            }
            std::string sel = singleNum ? a.positional[0] : a.positional[1];
            auto events = st.db->getEvents(tRoom, 300);
            std::vector<std::string> roots;
            for (const auto& ev : events) {
                int rc = 0;
                for (const auto& ev2 : events) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0) roots.push_back(ev.event_id);
            }
            std::string root;
            if (isNum(sel)) {
                int n = std::atoi(sel.c_str());
                if (n < 1 || n > static_cast<int>(roots.size())) {
                    st.statusNote = "thread #" + sel + " not found ("
                                  + std::to_string(roots.size()) + " threads)";
                    std::cout << drawFrameImpl(st) << std::flush;
                    return 1;
                }
                root = roots[static_cast<size_t>(n - 1)];
            } else {
                for (const auto& r : roots) {
                    if (r == sel) { root = r; break; }
                }
                if (root.empty()) {
                    for (const auto& r : roots) {
                        if (r.find(sel) != std::string::npos) { root = r; break; }
                    }
                }
                if (root.empty()) {
                    st.statusNote = "thread root not found: " + sel;
                    std::cout << drawFrameImpl(st) << std::flush;
                    return 1;
                }
            }
            st.threadRootId = root;
            st.threadReplies.clear();
            for (const auto& ev : events) {
                if (eventThreadRoot(ev) == root) st.threadReplies.push_back(ev);
            }
            st.rightPanel = 2;
            st.threadRoomId = tRoom;
            st.statusNote = "right panel: thread " + root.substr(0, 12)
                          + " (" + std::to_string(st.threadReplies.size()) + " replies)";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- threads: threads across ALL rooms (the Element thread list) ----
        if (a.command == "threads") {
            st.rightPanel = 3;
            st.statusNote = "right panel: threads in all rooms";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "rooms") {
            if (st.mobile) {
                st.mobileTab = 0;
                st.scroll = 0;
            }
            st.rooms = dbi.listRooms();
            sortRoomsByActivity(st);
            loadRoomIntoStateImpl(st, std::string(st.currentRoomId));
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "up" || a.command == "down" ||
            a.command == "top" || a.command == "bottom" ||
            a.command == "scroll") {
            int step = 5;
            if (a.positional.size() >= 1) {
                try { step = std::stoi(a.positional[0]); } catch (...) {}
            }
            if (a.command == "up") st.scroll -= std::max(1, step);
            else if (a.command == "down") st.scroll += std::max(1, step);
            else if (a.command == "top") st.scroll = 0;
            else if (a.command == "bottom") st.scroll = contentRowsImpl(st);
            else st.scroll += step;  // "scroll <n>" = down by n
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- names | aliases: the room list shows display titles or
        // #handles (persisted, both in the REPL and the static frame). ----
        // ---- only left|center|right|all: ONE panel at its full-frame width
        // (its own scroll applies); "all" restores the three-panel frame. ----
        if (a.command == "only") {
            const std::string& w = a.positional.empty() ? std::string("all")
                                                        : a.positional[0];
            if (w == "left") st.panelOnly = 1;
            else if (w == "center" || w == "middle") st.panelOnly = 2;
            else if (w == "right") st.panelOnly = 3;
            else st.panelOnly = 0;
            st.statusNote = st.panelOnly == 0
                                ? "panels: all" : "panel: " + w;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- absolute: toggle frame-slot positioning for panel-only output.
        if (a.command == "absolute") {
            st.panelAbsolute = !st.panelAbsolute;
            st.statusNote = st.panelAbsolute ? "pos: absolute" : "pos: flow";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "names" || a.command == "aliases") {
            st.roomNames = a.command == "names";
            dbi.setSetting("room_names", st.roomNames ? "1" : "0");
            st.statusNote = st.roomNames ? "room list: names"
                                         : "room list: aliases";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "spaces") {
            std::cout << "Spaces:" << std::endl;
            for (const auto& r : st.rooms) {
                if (!r.value("is_space", false)) continue;
                std::string id = r.value("room_id", "");
                std::cout << "  " << r.value("name", "?") << "  " << id
                          << std::endl;
            }
            if (st.mobile) {
                st.mobileTab = 0;
                st.scroll = 0;
                std::cout << drawFrameImpl(st) << std::flush;
            }
            return 1;
        }
        if (a.command == "space") {
            std::string q = a.positional.empty() ? "all" : a.positional[0];
            st.activeSpace.clear();
            if (q != "all" && q != "-") {
                st.activeSpace = resolveSpace(st.rooms, q);
            }
        }
        if (a.command == "chat" && st.mobile) {
            st.mobileTab = 1;
            st.scroll = 0;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "people" && st.mobile) {
            st.mobileTab = 2;
            st.scroll = 0;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "mobile") {
            std::string v = a.positional.empty() ? "on" : a.positional[0];
            st.mobile = (v == "on" || v == "1" || v == "true" || v == "yes");
            st.scroll = 0;
            st.statusNote = st.mobile
                ? "smartphone layout: stacked sections"
                : "desktop layout: three columns";
            dbi.setSetting("mobile", st.mobile ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "rows") {
            try {
                st.limitRows = std::stoi(a.positional.empty() ? "0" : a.positional[0]);
            } catch (...) { st.limitRows = 0; }
            st.scroll = 0;
            st.statusNote = st.limitRows > 0
                ? "frame height: " + std::to_string(st.limitRows) + " rows"
                : "frame height: auto (terminal)";
            dbi.setSetting("rows", std::to_string(st.limitRows));
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "refresh") {
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "attach" || a.command == "send-file") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: attach <room> <file> [--caption text]" << std::endl;
                return 1;
            }
            std::string roomQ = a.positional[0];
            std::string path = a.positional[1];
            std::string cap = a.options.count("caption") ? a.options.at("caption") : "";
            std::string thr = a.options.count("thread") ? a.options.at("thread") : "";
            // With a saved session the real upload+send runs; offline we
            // insert a local m.file message (demo/cache).
            auto cliHandler = CommandRegistry::instance().findCli("attach");
            bool hasSession = pcore::init() && pcore::loadSavedSession();
            if (hasSession && cliHandler) {
                cliHandler(a);
            } else {
                std::string roomId = roomQ;
                for (const auto& r : st.rooms) {
                    std::string id = r.value("room_id", "");
                    std::string name = r.value("name", "");
                    if (id == roomQ || name == roomQ ||
                        (!roomQ.empty() && (name.find(roomQ) == 0 ||
                                            id.find(roomQ) != std::string::npos))) {
                        roomId = id;
                        break;
                    }
                }
                uiInsertLocalFile(dbi, roomId, path, cap, thr);
                std::cout << "[offline] file recorded locally: " << path
                          << " -> " << roomId << std::endl;
            }
            loadRoomIntoStateImpl(st, std::string(st.currentRoomId));
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "send") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: send <room> <text>" << std::endl;
                return 1;
            }
            std::string room = a.positional[0];
            std::string body;
            for (size_t i = 1; i < a.positional.size(); ++i) {
                if (!body.empty()) body += " ";
                body += a.positional[i];
            }
            std::string roomId = room;
            for (const auto& r : st.rooms) {
                if (r.value("room_id", "") == room ||
                    r.value("name", "") == room) {
                    roomId = r.value("room_id", "");
                    break;
                }
            }
            int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            matrix::Event ev;
            ev.event_id = "$ui_" + std::to_string(ts);
            ev.room_id = roomId;
            ev.sender = "@you:local";
            ev.type = "m.room.message";
            ev.content = {{"body", body}, {"msgtype", "m.text"}};
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            if (roomId == st.currentRoomId) {
                loadRoomIntoStateImpl(st, roomId);
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- find: filter the room list ----
        if (a.command == "filter") {
            // The room-name filter (the left panel) — `find` is now the
            // message search.
            if (a.positional.empty()) {
                st.roomFilter.clear();
                st.statusNote = "room filter cleared";
            } else {
                st.roomFilter = a.positional[0];
                st.statusNote = "rooms matching '" + st.roomFilter + "'";
            }
            st.scroll = 0;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- space: like find, for spaces (rooms of a space by name) ----
        if (a.command == "space") {
            if (a.positional.empty() || a.positional[0] == "--all" || a.positional[0] == "all") {
                st.roomFilter.clear();
                st.statusNote = "all rooms";
            } else {
                st.roomFilter = a.positional[0];
                st.statusNote = "space/rooms matching '" + st.roomFilter + "'";
            }
            st.scroll = 0;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- modredact on|off: whether the client may redact OTHER
        // users' messages (the moderation) — off by default. Your OWN
        // messages are always redactable. ----
        if (a.command == "modredact") {
            const bool on = a.positional.size() >= 1 && a.positional[0] == "on";
            dbi.setSetting("mod_redact", on ? "1" : "0");
            st.statusNote = std::string("moderation redactions: ") + (on ? "allowed" : "blocked");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- powerlevels: the full power-level structure (the custom
        // user levels, the event overrides, the defaults) ----
        if (a.command == "powerlevels" || a.command == "pl") {
            if (st.powerLevelsEvent.empty() || st.powerLevelsEvent.is_null()) {
                std::cout << "No m.room.power_levels in the cache for this room."
                          << std::endl;
                return 1;
            }
            const auto& pl = st.powerLevelsEvent;
            std::string roomLabel = st.currentRoomId;
            for (const auto& r : st.rooms) {
                if (r.value("room_id", "") == st.currentRoomId) {
                    roomLabel = roomDisplayNameImpl(st, r);
                    break;
                }
            }
            std::cout << "Power levels — " << roomLabel << std::endl;
            std::cout << "  defaults: users " << pl.value("users_default", 0)
                      << " · events " << pl.value("events_default", 50)
                      << " · state " << pl.value("state_default", 50)
                      << " · invite " << pl.value("invite", 0)
                      << " · ban " << pl.value("ban", 50)
                      << " · kick " << pl.value("kick", 50)
                      << " · redact " << pl.value("redact", 50)
                      << std::endl;
            if (pl.contains("users") && pl["users"].is_object() &&
                !pl["users"].empty()) {
                std::vector<std::pair<int, std::string>> users;
                for (auto& [uid, lvl] : pl["users"].items()) {
                    if (lvl.is_number())
                        users.push_back({lvl.get<int>(), uid});
                }
                std::sort(users.begin(), users.end(),
                          [](const auto& x, const auto& y) { return x.first > y.first; });
                std::cout << "  the user levels:" << std::endl;
                for (const auto& [lvl, uid] : users) {
                    std::string badge = lvl >= 100 ? "admin"
                                      : lvl >= 50  ? "mod"
                                      : lvl > 0    ? "custom"
                                                   : "(none)";
                    std::cout << "    " << std::setw(4) << lvl << "  "
                              << (lvl > 0 && lvl < 50 ? "\x1b[36m" : "")
                              << uid << "\x1b[0m"
                              << (lvl > 0 && lvl < 50
                                      ? "  [custom]"
                                      : lvl > 0 ? "  [" + badge + "]" : "")
                              << std::endl;
                }
            }
            if (pl.contains("events") && pl["events"].is_object() &&
                !pl["events"].empty()) {
                std::cout << "  the event overrides:" << std::endl;
                for (auto& [etype, lvl] : pl["events"].items()) {
                    if (lvl.is_number())
                        std::cout << "    " << std::setw(4) << lvl.get<int>()
                                  << "  " << etype << std::endl;
                }
            }
            return 1;
        }
        // ---- accounts / account: multi-account switching ----
        if (a.command == "accounts") {
            auto cliHandler = CommandRegistry::instance().findCli("accounts");
            if (cliHandler) cliHandler(a);
            return 1;
        }
        if (a.command == "account" || a.command == "switch") {
            if (a.positional.empty()) {
                std::cout << "Usage: account <@user:server>  (see 'accounts')" << std::endl;
                return 1;
            }
            if (!pcore::init()) { std::cout << "No session store." << std::endl; return 1; }
            auto& core = pcore::core();
            auto all = core.store->listAccounts();
            bool found = false;
            for (const auto& acct : all) {
                if (acct.userId == a.positional[0]) {
                    if (!core.store->activateAccount(acct.userId)) {
                        std::cout << "Could not activate " << acct.userId << std::endl;
                        found = true;
                        break;
                    }
                    if (!pcore::loadSavedSession()) {
                        std::cout << "Could not reload the session." << std::endl;
                        found = true;
                        break;
                    }
                    std::string uid = acct.userId;
                    if (!uid.empty() && uid[0] == '@') uid = uid.substr(1);
                    st.accountLabel = uid;
                    st.rooms = dbi.listRooms();
                    loadRoomIntoStateImpl(st, "");
                    if (st.currentRoomId.empty() && !st.rooms.empty()) {
                        loadRoomIntoStateImpl(st, st.rooms.front().value("room_id", ""));
                    }
                    st.presence.clear();
                    st.statusNote = "account switched to " + uid;
                    found = true;
                    break;
                }
            }
            if (!found) std::cout << "No such account: " << a.positional[0] << std::endl;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- verify: SAS session verification (needs a session) ----
        if (a.command == "verify" || a.command == "verify-wait") {
            auto cliHandler = CommandRegistry::instance().findCli(a.command);
            if (!cliHandler) {
                std::cout << "verify not available in this build." << std::endl;
                return 1;
            }
            cliHandler(a);
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- presence: fetch member presence (needs a session) ----
        if (a.command == "presence") {
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "presence needs a logged-in session." << std::endl;
                return 1;
            }
            auto& core = pcore::core();
            auto client = core.client;
            st.presence.clear();
            for (const auto& mem : st.members) {
                auto r = client->getPresence(mem);
                std::string letter = "?";
                if (r.ok) {
                    try {
                        auto j = nlohmann::json::parse(r.data);
                        std::string p = j.value("presence", "");
                        if (p == "online") letter = "O";
                        else if (p == "busy" || p == "unavailable") letter = "A";
                        else if (p == "offline") letter = "F";
                    } catch (...) {}
                }
                st.presence[mem] = letter;
            }
            st.statusNote = "presence fetched (" + std::to_string(st.members.size()) + " members)";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- moderation + room settings (delegate to the registry) ----
        if (a.command == "ban" || a.command == "kick" || a.command == "unban") {
            auto cliHandler = CommandRegistry::instance().findCli(a.command);
            if (!cliHandler) {
                std::cout << a.command << " not available in this build." << std::endl;
                return 1;
            }
            cliHandler(a);
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- media: download + save/open a media event ----
    return 0;
}

} // namespace matrixcli

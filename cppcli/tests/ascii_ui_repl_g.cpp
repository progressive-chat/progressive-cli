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

int asciiReplDispatchG(UiState& st, db::Database& dbi, const cli::Args& a) {
        if (a.command == "reply") {
            // "reply 18:52 [bob] text" — the reference format (see below).
            auto colon = a.positional.empty() ? std::string::npos
                                              : a.positional[0].find(':');
            if (a.positional.size() >= 3 && colon != std::string::npos &&
                a.positional[0].size() == 5) {
                goto replyRef;
            }
            if (a.positional.size() < 3) {
                std::cout << "Usage: reply <room> <event_id> <text> |"
                             " reply <HH:MM> <[nick]> <text>" << std::endl;
                return 1;
            }
            auto cliHandler = CommandRegistry::instance().findCli("reply");
            if (!cliHandler) {
                std::cout << "reply not available in this build." << std::endl;
                return 1;
            }
            cliHandler(a);
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- jump: view the room starting from a date (like Element) ----
        if (a.command == "jump" || a.command == "date") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: jump <room> <YYYY-MM-DD> [--server]" << std::endl;
                std::cout << "  --server walks the server history (full room);"
                             " default = the cache" << std::endl;
                return 1;
            }
            int64_t dayMs = parseDayMs(a.positional[1]);
            if (dayMs < 0) {
                std::cout << "Bad date '" << a.positional[1] << "' (use YYYY-MM-DD)"
                          << std::endl;
                return 1;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == roomQ || id.find(roomQ) != std::string::npos ||
                    name == roomQ || name.find(roomQ) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            bool useServer = a.options.count("server") &&
                             pcore::init() && pcore::loadSavedSession();
            std::vector<matrix::Event> target;
            int64_t seen = 0;
            if (useServer) {
                // Walk the server history (newest -> oldest) until a batch
                // crosses the date; the crossing batch becomes the view.
                auto& core = pcore::core();
                auto client = core.client;
                std::string from = "";
                for (int guard = 0; guard < 2000; ++guard) {
                    auto r = client->getMessages(roomId, from, 100);
                    if (!r.ok) break;
                    std::string end;
                    try {
                        auto j = nlohmann::json::parse(r.data);
                        auto chunk = j.value("chunk", nlohmann::json::array());
                        for (const auto& ev : chunk) {
                            matrix::Event e;
                            e.event_id = ev.value("event_id", "");
                            e.sender = ev.value("sender", "");
                            e.type = ev.value("type", "");
                            e.origin_server_ts = ev.value("origin_server_ts", 0LL);
                            auto c = ev.find("content");
                            if (c != ev.end() && c->is_object()) e.content = *c;
                            target.push_back(std::move(e));
                        }
                        end = j.value("end", "");
                        // Crossed the date? The batch now contains the day.
                        bool crossed = false;
                        for (const auto& ev : chunk) {
                            if (ev.value("origin_server_ts", 0LL) < dayMs) crossed = true;
                        }
                        seen += static_cast<int>(chunk.size());
                        if (crossed || chunk.empty() || end.empty() || end == from) break;
                    } catch (...) { break; }
                    from = end;
                }
                // Newest-first (the walk order) -> chronological.
                std::reverse(target.begin(), target.end());
                // Cut to the events from the date onwards.
                size_t cut = 0;
                while (cut < target.size() &&
                       target[cut].origin_server_ts < dayMs) cut++;
                if (cut > 0) {
                    target.erase(target.begin(), target.begin() + cut);
                }
            } else {
                auto all = dbi.getEvents(roomId, 50000);
                std::reverse(all.begin(), all.end());  // newest-first
                size_t cut = 0;
                while (cut < all.size() && all[cut].origin_server_ts < dayMs) cut++;
                if (cut > 0) {
                    all.erase(all.begin(), all.begin() + cut);
                }
                std::reverse(all.begin(), all.end());  // chronological
                target = std::move(all);
            }
            st.currentRoomId = roomId;
            st.messages = std::move(target);
            st.scroll = 0;
            st.statusNote = "jumped to " + a.positional[1]
                          + (useServer ? " (server history)" : "") + " — "
                          + std::to_string(st.messages.size()) + " events from that day on";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- invite: invite a user into the room ----
        if (a.command == "invite") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: invite <room> <@user>" << std::endl;
                return 1;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "invite needs a logged-in session." << std::endl;
                return 1;
            }
            std::string roomId = a.positional[0];
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == roomId || id.find(roomId) != std::string::npos ||
                    name == roomId || name.find(roomId) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            auto r = pcore::core().client->inviteUser(roomId, a.positional[1]);
            if (!r.ok) {
                std::cout << "Invite failed: " << r.error.message << std::endl;
            } else {
                st.statusNote = "invited " + a.positional[1] + " to " + roomId;
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- create-room / create-space ----
        if (a.command == "create-room" || a.command == "create-space") {
            if (a.positional.empty()) {
                std::cout << "Usage: create-room <name> [--topic T] [--encrypted]"
                             " [--public] [--invite @u1,@u2]"
                          << std::endl;
                return 1;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "create-room needs a logged-in session." << std::endl;
                return 1;
            }
            std::string topic = a.options.count("topic") ? a.options.at("topic") : "";
            bool isPublic = a.options.count("public");
            bool encrypt = a.options.count("encrypted");
            std::vector<std::string> invites;
            if (a.options.count("invite")) {
                std::string csv = a.options.at("invite");
                std::string cur;
                for (char c : csv) {
                    if (c == ',') { if (!cur.empty()) invites.push_back(cur); cur.clear(); }
                    else cur += c;
                }
                if (!cur.empty()) invites.push_back(cur);
            }
            auto r = pcore::core().client->createRoom(a.positional[0], topic,
                                                      false, invites, encrypt);
            if (!r.ok) {
                std::cout << "Create failed: " << r.error.message << std::endl;
                return 1;
            }
            std::string roomId = r.data;
            std::string kind = a.command == "create-space" ? "space" : "room";
            st.statusNote = kind + " created: " + a.positional[0] + " (" + roomId + ")";
            std::cout << "Created " << kind << ": " << a.positional[0] << " (" << roomId
                      << ")" << std::endl;
            st.rooms = dbi.listRooms();
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- add-to-space: make a room a child of a space ----
        if (a.command == "add-to-space") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: add-to-space <room> <space>" << std::endl;
                return 1;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "add-to-space needs a logged-in session." << std::endl;
                return 1;
            }
            auto resolve = [&](const std::string& q) -> std::string {
                for (const auto& r : st.rooms) {
                    std::string id = r.value("room_id", "");
                    std::string name = r.value("name", "");
                    if (id == q || id.find(q) != std::string::npos ||
                        name == q || name.find(q) != std::string::npos) return id;
                }
                return q;
            };
            std::string roomId = resolve(a.positional[0]);
            std::string spaceId = resolve(a.positional[1]);
            std::string hs = pcore::core().client->account().homeserverUrl;
            std::string server = hs;
            auto hc = server.find("://");
            if (hc != std::string::npos) server = server.substr(hc + 3);
            auto slash = server.find('/');
            if (slash != std::string::npos) server = server.substr(0, slash);
            std::string viaJson = "{\"via\":[\"" + server + "\"]}";
            auto c1 = pcore::core().client->sendStateEvent(spaceId, "m.space.child",
                                                           roomId, viaJson);
            auto c2 = pcore::core().client->sendStateEvent(roomId, "m.room.parent",
                                                           spaceId, viaJson);
            if (!c1.ok && !c2.ok) {
                std::cout << "add-to-space failed: " << c1.error.message << std::endl;
            } else {
                st.statusNote = "room " + roomId + " added to space " + spaceId;
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- pin / unpin ----
        if ((a.command == "pin" || a.command == "unpin") &&
            !(a.positional.size() >= 2 && a.positional[0].size() == 5 &&
              a.positional[0][2] == ':')) {
            if (a.positional.size() < 2) {
                std::cout << "Usage: " << a.command << " <room> <event_id> |"
                             " pin <HH:MM> <[nick]>" << std::endl;
                return 1;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << a.command << " needs a logged-in session." << std::endl;
                return 1;
            }
            std::string roomId = a.positional[0];
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomId || id.find(roomId) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            bool ok = a.command == "pin"
                ? pcore::core().client->pinMessage(roomId, a.positional[1]).ok
                : pcore::core().client->unpinMessage(roomId, a.positional[1]).ok;
            st.statusNote = std::string(a.command) + " "
                          + (ok ? "ok" : "failed") + " in " + roomId;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- search filters: --sender, --since, --until ----
        if (a.command == "search" || a.command == "find") {
            if (a.positional.empty()) {
                std::cout << "Usage: find <query> [--limit N] [--sender @u]"
                             " [--since YYYY-MM-DD] [--until YYYY-MM-DD]\n"
                             "  The full-text search over the cached messages"
                             " — the bodies, the mxids, the reply context and"
                             " the matrix.to links.\n";
                return 1;
            }
            int limit = 10;
            if (a.options.count("limit")) {
                try { limit = std::stoi(a.options.at("limit")); } catch (...) {}
            }
            int64_t sinceMs = a.options.count("since") ? parseDayMs(a.options.at("since")) : -1;
            int64_t untilMs = a.options.count("until") ? parseDayMs(a.options.at("until")) : -1;
            std::string senderF = a.options.count("sender") ? a.options.at("sender") : "";
            auto hits = dbi.search(a.positional[0], std::max(limit, 5) * 8);
            int shown = 0;
            std::cout << std::endl;
            for (const auto& h : hits) {
                if (shown >= limit) break;
                if (!senderF.empty() && h.value("sender", "") != senderF) continue;
                const int64_t ts = h.value("origin_server_ts", 0LL);
                if (sinceMs > 0 && ts < sinceMs) continue;
                if (untilMs > 0 && ts > untilMs) continue;
                const std::string roomId = h.value("room_id", "");
                const std::string roomName = h.value("room_name", roomId);
                const std::string mxid = h.value("sender", "?");
                const std::string body =
                    h.value("content", nlohmann::json::object()).value("body", "");

                std::time_t t = ts / 1000;
                char tbuf[24];
                std::strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M", std::localtime(&t));
                std::cout << "  \x1b[1m" << roomName << "\x1b[0m  "
                          << senderShortImpl(mxid) << "  (\x1b[36m" << mxid
                          << "\x1b[0m)  " << tbuf << std::endl;
                // The full body, wrapped to the terminal.
                auto lines = wrapText(body, terminalWidthImpl() - 6);
                for (const auto& l : lines) std::cout << "    " << l << std::endl;

                // The reply context: the parent message when present.
                matrix::Event ev;
                if (dbi.getEventById(h.value("event_id", ""), ev) &&
                    ev.content.contains("m.relates_to")) {
                    const auto& rel = ev.content["m.relates_to"];
                    const std::string relType = rel.value("rel_type", "");
                    if (relType == "m.in_reply_to") {
                        const std::string parentId = rel.value("event_id", "");
                        matrix::Event parent;
                        if (dbi.getEventById(parentId, parent)) {
                            std::string pbody = parent.content.value("body", "");
                            if (pbody.size() > 120) pbody = pbody.substr(0, 120) + "...";
                            std::cout << "    \x1b[90m\u21b3 reply to "
                                      << senderShortImpl(parent.sender) << ": "
                                      << pbody << "\x1b[0m" << std::endl;
                        }
                    }
                }
                std::string eid = h.value("event_id", "");
                if (!eid.empty() && eid[0] == '$') eid = eid.substr(1);
                std::cout << "    \x1b[90m\U0001F517 https://matrix.to/#/"
                          << roomId << "/$" << eid
                          << "\x1b[0m" << std::endl << std::endl;
                shown++;
            }
            if (shown == 0) {
                std::cout << "No matches for '" << a.positional[0] << "'"
                          << (senderF.empty() ? "" : " from " + senderF) << std::endl;
            } else {
                std::cout << shown << " match" << (shown == 1 ? "" : "es")
                          << " for '" << a.positional[0] << "'"
                          << (senderF.empty() ? "" : " from " + senderF) << std::endl;
            }
            return 1;
        }
        // ---- goto: jump the chat viewport to an event ----
        if (a.command == "goto") {
            if (a.positional.empty()) {
                std::cout << "Usage: goto <event_id> | lastread | successor | newest (back to the latest)"
                          << std::endl;
                return 1;
            }
            std::string q = a.positional[0];
            if (q == "lastread" || q == "last-read" || q == "unread") {
                // Jump to the last-read position (the local m.fully_read
                // copy), right after the marker.
                if (st.readMarker.empty()) {
                    st.statusNote = "no last-read marker (read <room> sets it)";
                    std::cout << drawFrameImpl(st) << std::flush;
                    return 1;
                }
                q = st.readMarker;
            }
            if (q == "successor") {
                // The room was upgraded: follow the tombstone's successor.
                std::string succ = tombstoneSuccessor(st.messages);
                if (succ.empty()) {
                    st.statusNote = "no tombstone in this room — nothing to follow";
                    std::cout << drawFrameImpl(st) << std::flush;
                    return 1;
                }
                loadRoomIntoStateImpl(st, succ);
                st.statusNote = "followed the upgrade \xe2\x86\x92 " + succ;
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            matrix::Event target;
            if (!st.db->getEventById(q, target)) {
                st.statusNote = "event not in the cache: " + q;
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            if (st.currentRoomId != target.room_id) {
                loadRoomIntoStateImpl(st, target.room_id);
            }
            bool inWindow = std::find_if(
                st.messages.begin(), st.messages.end(),
                [&](const matrix::Event& ev) { return ev.event_id == q; }) !=
                st.messages.end();
            if (!inWindow) {
                st.limit = 5000;  // the event is older than the window
                loadRoomIntoStateImpl(st, target.room_id);
                inWindow = std::find_if(
                    st.messages.begin(), st.messages.end(),
                    [&](const matrix::Event& ev) { return ev.event_id == q; }) !=
                    st.messages.end();
            }
            if (!inWindow) {
                st.statusNote = "event exists but is outside the loaded window";
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            st.focusEvent = q;
            int rowIdx = centerRowIndexOf(st, q);
            st.scroll = rowIdx >= 0 ? std::max(0, rowIdx - 12) : 0;
            if (st.mobile) st.mobileTab = 1;
            st.statusNote = "viewing event ‹" + q + "› · 'newest' to return";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        if (a.command == "newest") {
            st.focusEvent.clear();
            st.scroll = 1 << 30;  // clamped to the bottom in drawFrame
            st.statusNote = "back to the latest messages";
            if (st.mobile) st.mobileTab = 1;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- threads bottom on|off: the thread list in the right panel ----
        if (a.command == "threads" && a.positional.size() >= 2 &&
            a.positional[0] == "bottom") {
            st.showThreadsBottom = (a.positional[1] != "off");
            dbi.setSetting("threads_bottom", st.showThreadsBottom ? "1" : "0");
            st.statusNote = std::string("thread list in the right panel ") +
                            (st.showThreadsBottom ? "on" : "off");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- pins: the room's pinned messages (with a goto hint) ----
        if (a.command == "pins") {
            if (st.pinned.empty()) {
                std::cout << "No pinned messages in " << st.currentRoomId
                          << ". (pin <event_id> pins the current view — the "
                          << "list updates when a client sets m.room.pinned_events.)"
                          << std::endl;
                return 1;
            }
            int n = 0;
            for (const auto& ev : st.messages) {
                if (!st.pinned.count(ev.event_id)) continue;
                n++;
                std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000)
                              + static_cast<std::time_t>(st.tzOffset) * 3600;
                std::tm tm{};
                localtime_r(&t, &tm);
                char dateBuf[16];
                std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
                std::string body = eventBodyImpl(ev);
                std::string shortId = ev.event_id.substr(0, 10);
                std::cout << n << ". " << dateBuf << " " << senderShortImpl(ev.sender)
                          << " \x1b[90m\u2039" << shortId << "\u203a\x1b[0m "
                          << clip(body, 60) << std::endl;
            }
            if (n < static_cast<int>(st.pinned.size())) {
                std::cout << (st.pinned.size() - n)
                          << " more pinned message(s) outside the loaded window."
                          << std::endl;
            }
            std::cout << "\nJump: goto <short-id> \u00b7 unpin <short-id>" << std::endl;
            return 1;
        }
        // ---- files: the room's media messages (audio/video/image/file) ----
        if (a.command == "files") {
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == q || id.find(q) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            auto evs = dbi.getEvents(roomId, 300);
            int shown = 0;
            std::cout << "Media in " << roomId << ":" << std::endl;
            for (const auto& ev : evs) {
                if (!ev.content.is_object()) continue;
                std::string mt = ev.content.value("msgtype", "");
                std::string icon;
                if (mt == "m.audio") icon = "\xf0\x9f\x8e\xb5 audio";
                else if (mt == "m.video") icon = "\xe2\x96\xb6 video";
                else if (mt == "m.image") icon = "\xf0\x9f\x96\xbc image";
                else if (mt == "m.file") icon = "\xf0\x9f\x93\x84 file";
                else continue;
                std::string name = ev.content.value("body",
                                     ev.content.value("filename", "?"));
                std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000);
                std::tm tm{};
                localtime_r(&t, &tm);
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
                std::cout << "  " << icon << ": " << clip(name, 40)
                          << "  [" << senderShortImpl(ev.sender) << " " << buf << "]"
                          << "\n      id: " << ev.event_id;
                std::string mxc;
                auto urlIt = ev.content.find("url");
                if (urlIt != ev.content.end() && urlIt->is_string()) {
                    mxc = urlIt->get<std::string>();
                }
                if (!mxc.empty()) {
                    std::cout << "\n      mxc: " << mxc;
                    // mxc://host/id -> the standard download URL.
                    auto hostEnd = mxc.find('/', 6);
                    if (mxc.compare(0, 6, "mxc://") == 0 &&
                        hostEnd != std::string::npos) {
                        std::string host = mxc.substr(6, hostEnd - 6);
                        std::string mediaId = mxc.substr(hostEnd + 1);
                        std::cout << "\n      url: https://" << host
                                  << "/_matrix/media/v3/download/" << host << "/"
                                  << mediaId;
                    }
                }
                std::cout << "\n      open: media " << roomId << " " << ev.event_id
                          << " --open\n";
                shown++;
            }
            if (shown == 0) {
                std::cout << "  No media in this room." << std::endl;
            }
            return 1;
        }
        // ---- widths: custom per-character widths (fix the pipes) ----

replyRef:
        if (a.command == "reply" || a.command == "react" ||
            a.command == "thread" || a.command == "pin" ||
            a.command == "copy") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: " << a.command
                          << " <HH:MM> <[nick]> [text|emoji]" << std::endl;
                return 1;
            }
            // Parse the reference.
            int hh = 0, mm = 0;
            bool badRef = false;
            {
                auto colon = a.positional[0].find(':');
                if (colon == std::string::npos) badRef = true;
                else {
                    try {
                        hh = std::stoi(a.positional[0].substr(0, colon));
                        mm = std::stoi(a.positional[0].substr(colon + 1));
                    } catch (...) { badRef = true; }
                }
            }
            if (badRef) {
                std::cout << "Usage: " << a.command << " <HH:MM> <[nick]> [text]"
                          << std::endl;
                return 1;
            }
            std::string nick = a.positional[1];
            if (!nick.empty() && nick.front() == '[' && nick.back() == ']') {
                nick = nick.substr(1, nick.size() - 2);
            }
            const matrix::Event* target = nullptr;
            for (auto it = st.messages.rbegin(); it != st.messages.rend(); ++it) {
                std::time_t t = static_cast<std::time_t>(it->origin_server_ts / 1000)
                              + static_cast<std::time_t>(st.tzOffset) * 3600;
                std::tm tm{};
                localtime_r(&t, &tm);
                if (tm.tm_hour == hh && tm.tm_min == mm &&
                    senderShortImpl(it->sender) == nick) {
                    target = &(*it);
                    break;
                }
            }
            if (!target) {
                std::cout << "No message matching " << a.positional[0] << " ["
                          << nick << "] in the window." << std::endl;
                return 1;
            }
            std::string roomId = st.currentRoomId;
            int64_t nowTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (a.command == "copy") {
                std::cout << "https://matrix.to/#/" << roomId << "/"
                          << target->event_id
                          << viaSuffix(&dbi, roomId, st.viaLimit) << std::endl;
                return 1;
            }
            if (a.command == "pin") {
                matrix::Event pin;
                pin.event_id = "$demo_pin_" + std::to_string(nowTs);
                pin.room_id = roomId; pin.sender = "@you";
                pin.type = "m.room.pinned_events";
                pin.state_key = "";
                pin.content = {{"pinned", nlohmann::json::array({target->event_id})}};
                pin.origin_server_ts = nowTs;
                dbi.insertEvent(pin);
                st.pinned.insert(target->event_id);
                st.statusNote = "pinned " + target->event_id;
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            if (a.positional.size() < 3) {
                std::cout << "Usage: " << a.command << " <HH:MM> <[nick]> <text>"
                          << std::endl;
                return 1;
            }
            std::string text = a.positional[2];
            matrix::Event act;
            act.event_id = "$demo_" + std::to_string(nowTs);
            act.room_id = roomId; act.sender = "@you";
            act.type = "m.room.message";
            act.origin_server_ts = nowTs;
            if (a.command == "react") {
                act.type = "m.reaction";
                act.content = {{"m.relates_to",
                                {{"event_id", target->event_id},
                                 {"rel_type", "m.annotation"},
                                 {"key", text}}}};
            } else if (a.command == "thread") {
                std::string root = eventThreadRoot(*target);
                std::string rootId = root.empty() ? target->event_id : root;
                act.content = {{"msgtype", "m.text"}, {"body", text},
                               {"m.relates_to", {{"event_id", rootId},
                                                 {"rel_type", "m.thread"}}}};
            } else {  // reply
                act.content = {{"msgtype", "m.text"}, {"body", text},
                               {"m.relates_to",
                                {{"m.in_reply_to", {{"event_id", target->event_id}}}}}};
            }
            dbi.insertEvent(act);
            st.statusNote = std::string(a.command) + " to "
                          + a.positional[0] + " [" + nick + "]";
            loadRoomIntoStateImpl(st, std::string(st.currentRoomId));
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- star <room> on|off: anchor the room to the top of the list ----
        if (a.command == "star" || a.command == "anchor") {
            if (a.positional.empty()) {
                std::cout << "Usage: " << a.command << " <room> on|off" << std::endl;
                return 1;
            }
            std::string q = a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == q || name == q || name.find(q) == 0 ||
                    id.find(q) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            bool on = a.positional.size() < 2 || a.positional[1] != "off";
            if (on) st.starredRooms.insert(roomId);
            else st.starredRooms.erase(roomId);
            std::string saved;
            for (const auto& id : st.starredRooms) {
                if (!saved.empty()) saved += ",";
                saved += id;
            }
            dbi.setSetting("starred", saved);
            sortRoomsByActivity(st);
            fprintf(stderr, "DBG star: first=%s starred=%zu\n",
                    st.rooms.empty() ? "?" : st.rooms.front().value("name","").c_str(),
                    st.starredRooms.size());
            st.statusNote = roomId + (on ? " ★ starred (top)" : " unstarred");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- roomname <room> <name> / topic <room> <text>: like Element ----
        if (a.command == "roomname" || a.command == "topic") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: " << a.command << " <room> <value>"
                          << std::endl;
                return 1;
            }
            std::string q = a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == q || name == q || name.find(q) == 0 ||
                    id.find(q) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            std::string value = a.positional[1];
            for (size_t pi = 2; pi < a.positional.size(); ++pi) {
                value += " " + a.positional[pi];
            }
            for (const auto& r : st.rooms) {
                if (r.value("room_id", "") != roomId) continue;
                nlohmann::json j;
                j["name"] = a.command == "roomname"
                                ? value : r.value("name", roomId);
                j["topic"] = a.command == "topic"
                                 ? value : r.value("topic", "");
                j["member_count"] = r.value("member_count", 0);
                j["is_direct"] = r.value("is_direct", false);
                j["is_encrypted"] = r.value("is_encrypted", false);
                dbi.upsertRoom(j, roomId);
                break;
            }
            st.rooms = dbi.listRooms();
            sortRoomsByActivity(st);
            loadRoomIntoStateImpl(st, std::string(st.currentRoomId));
            st.statusNote = roomId + ": " + std::string(a.command) + " = " + value;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- power <room>: the power levels; power <room> <@user> <level>;
        // power <room> default <level> (send permission) ----
        if (a.command == "power") {
            if (a.positional.empty()) {
                std::cout << "Usage: power <room> [<@user> <level>|default <level>]"
                          << std::endl;
                return 1;
            }
            std::string q = a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == q || name == q || name.find(q) == 0 ||
                    id.find(q) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            auto evs = dbi.getEvents(roomId, 500);
            int eventsDefault = 0;
            for (const auto& ev : evs) {
                if (ev.type == "m.room.power_levels") {
                    eventsDefault = ev.content.value("events_default", 0);
                    break;
                }
            }
            if (a.positional.size() < 3) {
                std::cout << "Power levels of " << roomId << ":" << std::endl;
                std::cout << "  events_default (send): " << eventsDefault
                          << std::endl;
                for (const auto& ev : evs) {
                    if (ev.type != "m.room.power_levels") continue;
                    auto users = ev.content.find("users");
                    if (users == ev.content.end() || !users->is_object()) continue;
                    for (auto& [u, lvl] : users->items()) {
                        std::cout << "  " << u << " = "
                                  << lvl.get<int>() << std::endl;
                    }
                    break;
                }
                return 1;
            }
            int level = 0;
            try { level = std::stoi(a.positional[2]); } catch (...) {
                std::cout << "Usage: power <room> <@user> <level>"
                          << std::endl;
                return 1;
            }
            // Insert/update the power levels event (offline demo: local).
            bool found = false;
            for (auto& ev : evs) {
                if (ev.type != "m.room.power_levels") continue;
                found = true;
                nlohmann::json users = ev.content.value("users", nlohmann::json::object());
                if (a.positional[1] == "default") {
                    ev.content["events_default"] = level;
                } else {
                    std::string u = a.positional[1];
                    if (u[0] != '@') u = "@" + u;
                    users[u] = level;
                    ev.content["users"] = users;
                }
                ev.origin_server_ts = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                dbi.insertEvent(ev);
                break;
            }
            if (!found) {
                matrix::Event pl;
                pl.event_id = "$demo_power_" + std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                pl.room_id = roomId; pl.sender = "@you";
                pl.type = "m.room.power_levels";
                pl.content = {{"events_default", eventsDefault}};
                if (a.positional[1] != "default") {
                    std::string u = a.positional[1];
                    if (u[0] != '@') u = "@" + u;
                    pl.content["users"] = {{u, level}};
                } else {
                    pl.content["events_default"] = level;
                }
                pl.origin_server_ts = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                dbi.insertEvent(pl);
            }
            loadRoomIntoStateImpl(st, std::string(st.currentRoomId));
            st.statusNote = "power: " + roomId + " " + a.positional[1]
                          + " = " + std::to_string(level);
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- links: list the matrix.to permalinks of the current room ----
        if (a.command == "links") {
            const std::string marker = "matrix.to/#/";
            int found = 0;
            std::cout << "Permalinks in " << st.currentRoomId << ":" << std::endl;
            for (const auto& ev : st.messages) {
                std::string body = eventBodyRaw(ev);
                size_t pos = 0;
                while ((pos = body.find(marker, pos)) != std::string::npos) {
                    size_t linkStart = pos;
                    if (linkStart >= 8 && body.compare(linkStart - 8, 8, "https://") == 0) {
                        linkStart -= 8;
                    } else if (linkStart >= 7 && body.compare(linkStart - 7, 7, "http://") == 0) {
                        linkStart -= 7;
                    }
                    size_t after = pos + marker.size();
                    size_t slash = body.find('/', after);
                    std::string roomPart = body.substr(after, slash == std::string::npos
                                                               ? std::string::npos
                                                               : slash - after);
                    std::string evPart = slash == std::string::npos
                                             ? "" : body.substr(slash + 1);
                    size_t spanEnd = slash == std::string::npos
                                         ? body.size() : slash + 1 + evPart.size();
                    std::string url = body.substr(linkStart, spanEnd - linkStart);
                    std::string roomName = roomPart;
                    for (const auto& r : st.rooms) {
                        if (r.value("room_id", "") == roomPart) {
                            roomName = r.value("name", roomPart);
                            break;
                        }
                    }
                    std::cout << "  " << (st.showEmoji ? "\xf0\x9f\x93\x8e " : "[pill] ")
                              << roomName;
                    if (!evPart.empty()) {
                        std::cout << " -> " << evPart;
                        matrix::Event target;
                        if (st.db->getEventById(evPart, target)) {
                            std::cout << "  [" << senderShortImpl(target.sender) << ": "
                                      << clip(eventBody(target), 40) << "]";
                        } else {
                            std::cout << "  (event not in the cache)";
                        }
                    }
                    std::cout << "\n      url: " << url
                              << viaSuffix(st.db, roomPart, st.viaLimit)
                              << "\n      read: raw " << roomPart << " " << evPart
                              << "\n      jump: goto " << evPart
                              << "\n";
                    found++;
                    pos = spanEnd;
                }
            }
            if (found == 0) {
                std::cout << "No permalinks in this room." << std::endl;
            }
            return 1;
        }
        // ---- voice: record (arecord/parec) + send as m.audio ----
        if (a.command == "voice") {
            if (a.positional.empty()) {
                std::cout << "Usage: voice <room> [--seconds N] [--out file]" << std::endl;
                return 1;
            }
            int seconds = 5;
            if (a.options.count("seconds")) {
                try { seconds = std::stoi(a.options.at("seconds")); } catch (...) {}
            }
            std::string tmp = a.options.count("out") ? a.options.at("out")
                                                     : "/tmp/matrixcli_voice.wav";
            std::string rec = "arecord -q -f S16_LE -r 44100 -c 1 -d "
                            + std::to_string(seconds) + " '" + tmp + "' 2>/dev/null || "
                            + "parec --record --format=s16le --rate=44100 --channels=1 "
                            + "2>/dev/null | sox -t raw -r 44100 -e signed -b 16 -c 1 - "
                            + "'" + tmp + "' 2>/dev/null";
            std::cout << "Recording " << seconds << "s to " << tmp << " ..." << std::endl;
            int rc = std::system(rec.c_str());
            if (rc != 0 || !std::filesystem::exists(tmp) ||
                std::filesystem::file_size(tmp) < 1000) {
                std::cout << "Recording failed (need arecord or parec+sox)." << std::endl;
                return 1;
            }
            cli::Args at;
            at.positional.push_back(a.positional[0]);
            at.positional.push_back(tmp);
            at.options["caption"] = "voice message";
            auto attachH = CommandRegistry::instance().findCli("attach");
            if (attachH) {
                attachH(at);
                st.statusNote = "voice message sent to " + a.positional[0];
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- sticker: send an m.sticker ----
        if (a.command == "sticker") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: sticker <room> <name> [--url mxc]" << std::endl;
                return 1;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "sticker needs a logged-in session." << std::endl;
                return 1;
            }
            std::string roomId = a.positional[0];
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomId || id.find(roomId) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            std::string url = a.options.count("url") ? a.options.at("url")
                                                     : "mxc://demo.local/sticker";
            std::string content = "{\"body\":\"" + a.positional[1]
                                + "\",\"url\":\"" + url + "\"}";
            auto r = pcore::core().client->sendMessageEvent(roomId, "m.sticker", content);
            if (!r.ok) {
                std::cout << "Sticker send failed: " << r.error.message << std::endl;
            } else {
                st.statusNote = "sticker sent to " + roomId;
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- notify: terminal/desktop notification for a room ----
        if (a.command == "notify") {
            std::string roomQ = a.positional.empty() ? st.currentRoomId : a.positional[0];
            std::string rname = roomQ;
            std::string last;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomQ || id.find(roomQ) != std::string::npos ||
                    roomQ == st.currentRoomId) {
                    rname = r.value("name", rname);
                    break;
                }
            }
            if (!st.messages.empty()) last = eventBody(st.messages.back());
            std::string cmd = "notify-send 'progressive-cli: " + rname + "' '" + clip(last, 80)
                            + "' 2>/dev/null || echo -e '\a'";
            std::system(cmd.c_str());
            st.statusNote = "notified for " + rname;
            return 1;
        }
    return 0;
}

} // namespace matrixcli

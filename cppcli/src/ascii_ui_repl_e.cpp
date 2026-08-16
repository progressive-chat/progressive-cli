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

int asciiReplDispatchE(UiState& st, db::Database& dbi, const cli::Args& a) {
        if (a.command == "widths") {
            if (a.positional.empty()) {
                if (g_widthOverrides.empty()) {
                    std::cout << "No custom widths. Usage: widths <char> <1|2>"
                              << std::endl;
                } else {
                    for (const auto& [cp, wd] : g_widthOverrides) {
                        std::string glyph = "?";
                        if (cp < 0x80) {
                            glyph = std::string(1, static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            glyph = std::string(1, static_cast<char>(0xC0 | (cp >> 6)))
                                  + static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            glyph = std::string(1, static_cast<char>(0xE0 | (cp >> 12)))
                                  + static_cast<char>(0x80 | ((cp >> 6) & 0x3F))
                                  + static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            glyph = std::string(1, static_cast<char>(0xF0 | (cp >> 18)))
                                  + static_cast<char>(0x80 | ((cp >> 12) & 0x3F))
                                  + static_cast<char>(0x80 | ((cp >> 6) & 0x3F))
                                  + static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "U+%04X", cp);
                        std::cout << "  " << glyph << " " << buf << " = "
                                  << wd << " cell" << (wd == 1 ? "" : "s")
                                  << std::endl;
                    }
                    std::cout << "Usage: widths <char> <1|2> | widths reset"
                              << std::endl;
                }
                return 1;
            }
            if (a.positional[0] == "reset") {
                g_widthOverrides.clear();
                dbi.setSetting("widths", "");
                st.statusNote = "custom widths cleared";
                std::cout << drawFrameImpl(st) << std::flush;
                return 1;
            }
            if (a.positional.size() < 2) {
                std::cout << "Usage: widths <char> <1|2>" << std::endl;
                return 1;
            }
            uint32_t cp = utf8FirstCp(a.positional[0]);
            int wd = 0;
            try { wd = std::stoi(a.positional[1]); } catch (...) {}
            if (cp == 0 || (wd != 1 && wd != 2)) {
                std::cout << "Usage: widths <char> <1|2>" << std::endl;
                return 1;
            }
            g_widthOverrides[cp] = wd;
            std::string saved;
            for (const auto& [c, w] : g_widthOverrides) {
                if (!saved.empty()) saved += ",";
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%X:%d", c, w);
                saved += buf;
            }
            dbi.setSetting("widths", saved);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "U+%04X = %d cell", cp, wd);
            st.statusNote = std::string("width ") + a.positional[0] + " (" + buf
                            + (wd == 1 ? ")" : "s)");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- permalink <room> <event_id>: the matrix.to link with via ----
replyRef:        if (a.command == "permalink") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: permalink <room> <event_id>" << std::endl;
                return 1;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomQ || id.find(roomQ) != std::string::npos ||
                    r.value("name", "") == roomQ) {
                    roomId = id;
                    break;
                }
            }
            matrix::Event ev;
            if (!dbi.getEventById(a.positional[1], ev)) {
                std::cout << "Event not found in the cache." << std::endl;
                return 1;
            }
            std::cout << "https://matrix.to/#/" << roomId << "/" << ev.event_id
                      << viaSuffix(&dbi, roomId, st.viaLimit) << std::endl;
            return 1;
        }
        // ---- via <n>: the via argument count in permalinks (0 = all) ----
        if (a.command == "via") {
            int v = 3;
            if (a.positional.empty()) {
                std::cout << "via: " << (st.viaLimit == 0 ? "unlimited (all servers)"
                                                          : std::to_string(st.viaLimit))
                          << "  (via <n> | via 0 = no limit)" << std::endl;
                return 1;
            }
            try { v = std::stoi(a.positional[0]); } catch (...) { v = -1; }
            if (v < 0) {
                std::cout << "Usage: via <n> | via 0 (unlimited)" << std::endl;
                return 1;
            }
            st.viaLimit = v;
            dbi.setSetting("via_limit", std::to_string(v));
            st.statusNote = std::string("permalinks via: ") +
                            (v == 0 ? "unlimited (all servers)" : std::to_string(v));
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- from <@user> | from off: only that sender's messages ----
        if (a.command == "from") {
            if (a.positional.empty() || a.positional[0] == "off" ||
                a.positional[0] == "all") {
                st.senderFilter.clear();
                st.statusNote = "showing all messages";
            } else {
                st.senderFilter = a.positional[0];
                if (st.senderFilter[0] != '@') st.senderFilter = "@" + st.senderFilter;
                st.statusNote = "showing only " + st.senderFilter;
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- hide [room] [seconds]: temporarily hide a room ----
        if (a.command == "hide") {
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == q || id.find(q) != std::string::npos || name == q) {
                    roomId = id;
                    break;
                }
            }
            int secs = st.hiddenSeconds;
            if (a.positional.size() >= 2) {
                try { secs = std::stoi(a.positional[1]); } catch (...) {}
            }
            if (secs <= 0) {
                st.hiddenRooms.erase(roomId);
                st.hiddenUntil.erase(roomId);
                st.statusNote = roomId + " unhidden";
            } else {
                st.hiddenRooms.insert(roomId);
                st.hiddenUntil[roomId] =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()
                    + int64_t(secs) * 1000;
                st.statusNote = roomId + " hidden for " + std::to_string(secs) + "s";
            }
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- mute <room> on|off: no indicators, just stay ----
        if (a.command == "mute") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: mute <room> on|off" << std::endl;
                return 1;
            }
            std::string q = a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == q || id.find(q) != std::string::npos ||
                    r.value("name", "") == q) {
                    roomId = id;
                    break;
                }
            }
            bool on = a.positional[1] != "off";
            if (on) st.mutedRooms.insert(roomId);
            else st.mutedRooms.erase(roomId);
            std::string saved;
            for (const auto& id : st.mutedRooms) {
                if (!saved.empty()) saved += ",";
                saved += id;
            }
            dbi.setSetting("muted", saved);
            st.statusNote = roomId + (on ? " muted (no indicators)" : " unmuted");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- timezone <N>: hours offset for all displayed times ----
        if (a.command == "timezone") {
            if (a.positional.empty()) {
                std::cout << "timezone: " << (st.tzOffset >= 0 ? "+" : "")
                          << st.tzOffset << "h  (timezone <N> | timezone 0)"
                          << std::endl;
                return 1;
            }
            try { st.tzOffset = std::stoi(a.positional[0]); } catch (...) { st.tzOffset = 0; }
            dbi.setSetting("tz_offset", std::to_string(st.tzOffset));
            st.statusNote = std::string("timezone UTC") + (st.tzOffset >= 0 ? "+" : "")
                          + std::to_string(st.tzOffset);
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- nick <room> <@user> <name>: per-room display names ----
        if (a.command == "nick") {
            if (a.positional.size() < 3) {
                std::cout << "Usage: nick <room> <@user> <name>" << std::endl;
                return 1;
            }
            std::string roomId = a.positional[0];
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == roomId || name == roomId ||
                    name.find(roomId) == 0 || id.find(roomId) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            std::string user = a.positional[1];
            if (user[0] != '@') user = "@" + user;
            std::string name = a.positional[2];
            st.roomNicks[roomId + "|" + user] = name;
            std::string saved;
            for (const auto& [k, v] : st.roomNicks) {
                if (!saved.empty()) saved += ",";
                saved += k + "=" + v;
            }
            dbi.setSetting("room_nicks", saved);
            st.statusNote = "nick: " + user + " = " + name + " (in " + roomId + ")";
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- avatar <room> <url>: per-room avatar (shown in info) ----
        if (a.command == "avatar") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: avatar <room> <mxc|url>" << std::endl;
                return 1;
            }
            std::string roomId = a.positional[0];
            for (const auto& r : st.rooms) {
                if (r.value("room_id", "") == roomId ||
                    r.value("name", "") == roomId) {
                    roomId = r.value("room_id", "");
                    break;
                }
            }
            st.roomAvatars[roomId] = a.positional[1];
            std::string saved;
            for (const auto& [k, v] : st.roomAvatars) {
                if (!saved.empty()) saved += ",";
                saved += k + "=" + v;
            }
            dbi.setSetting("room_avatars", saved);
            st.statusNote = "avatar set for " + roomId;
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- color <@user> <color|off>: custom nickname highlight ----
        if (a.command == "color") {
            if (a.positional.empty()) {
                std::cout << "Usage: color <@user> <red|green|yellow|blue|magenta|cyan|off>"
                          << std::endl;
                return 1;
            }
            std::string user = a.positional[0];
            if (user[0] != '@') user = "@" + user;
            if (a.positional.size() < 2 || a.positional[1] == "off") {
                st.userColors.erase(user);
            } else {
                st.userColors[user] = a.positional[1];
            }
            std::string saved;
            for (const auto& [k, v] : st.userColors) {
                if (!saved.empty()) saved += ",";
                saved += k + "=" + v;
            }
            dbi.setSetting("user_colors", saved);
            st.statusNote = std::string("color ") + user + " "
                          + (st.userColors.count(user) ? st.userColors[user] : "off");
            std::cout << drawFrameImpl(st) << std::flush;
            return 1;
        }
        // ---- info <room>: members, version, avatar, member search ----
        if (a.command == "info") {
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            std::string roomId = q;
            std::string roomName = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == q || id.find(q) != std::string::npos ||
                    r.value("name", "") == q || r.value("name", "").find(q) == 0) {
                    roomId = id;
                    roomName = r.value("name", "");
                    break;
                }
            }
            std::cout << "Room: " << roomName << " (" << roomId << ")" << std::endl;
            auto it = st.roomAvatars.find(roomId);
            std::cout << "  avatar: " << (it != st.roomAvatars.end() ? it->second
                                                                     : "(none)")
                      << std::endl;
            std::cout << "  version: demo.db (local cache)" << std::endl;
            std::string membersQ = a.positional.size() >= 2 ? a.positional[1] : "";
            auto evs = dbi.getEvents(roomId, 500);
            std::vector<std::string> members;
            for (const auto& ev : evs) {
                if (std::find(members.begin(), members.end(), ev.sender) ==
                    members.end()) {
                    members.push_back(ev.sender);
                }
            }
            int shown = 0;
            for (const auto& mem : members) {
                if (!membersQ.empty() &&
                    mem.find(membersQ) == std::string::npos) continue;
                std::string nm = displayName(st, roomId, mem);
                std::cout << "  " << nm;
                auto nickIt = st.roomNicks.find(roomId + "|" + mem);
                if (nickIt != st.roomNicks.end()) {
                    std::cout << " (" << senderShortImpl(mem) << ")";
                }
                std::cout << std::endl;
                shown++;
            }
            std::cout << "  members: " << members.size()
                      << (membersQ.empty() ? "" : " (filter: '" + membersQ + "')")
                      << std::endl;
            return 1;
        }
        // ---- reply/react/thread/pin/copy "HH:MM [nick]" — find the last
        // matching message and act on it ----
    return 0;
}

} // namespace matrixcli

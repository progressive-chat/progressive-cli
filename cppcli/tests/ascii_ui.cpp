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

int terminalWidthImpl() {
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) {
        return static_cast<int>(ws.ws_col);
    }
#endif
    return 100;
}

// Per-character width overrides (the "widths" setting): the phone
// terminal renders some glyphs narrower/wider than the default table
// (👑 🛡 ⤷ ❤ …), which shifted the pipes. The user can fix any of them.
// The first UTF-8 codepoint of a string (for the widths command).
uint32_t utf8FirstCp(const std::string& s) {
    if (s.empty()) return 0;
    unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) return c;
    size_t len = 0;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (s.size() < len) return 0;
    uint32_t cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
    for (size_t i = 1; i < len; ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    }
    return cp;
}

// Approximate terminal display width of one UTF-8 codepoint: 2 for CJK,
// emoji and misc symbols, 0 for combining marks, else 1.
int cpWidth(uint32_t cp) {
    auto it = g_widthOverrides.find(cp);
    if (it != g_widthOverrides.end()) return it->second;
    if (cp >= 0x0300 && cp <= 0x036F) return 0;      // combining
    if (cp == 0xFE0F || cp == 0xFE0E) return 0;      // variation selectors
    if (cp >= 0x1100 && cp <= 0x115F) return 2;
    if (cp >= 0x2E80 && cp <= 0xA4CF) return 2;      // CJK
    if (cp >= 0xAC00 && cp <= 0xD7A3) return 2;
    if (cp >= 0xF900 && cp <= 0xFAFF) return 2;
    if (cp >= 0xFE30 && cp <= 0xFE4F) return 2;
    if (cp >= 0xFF00 && cp <= 0xFF60) return 2;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2;
    // Misc symbols and dingbats (❤ ♪ ★ ⤷ ⭕ …) render ONE cell in the
    // phone terminal (glibc wcwidth agrees) — counting them 2 broke the
    // pipes. Emoji render two cells there (only the glyphs that show a
    // narrow/outline form are tuned below).
    if (cp == 0x1F451 || cp == 0x1F6E1) return 1;   // 👑 🛡 power badges are narrow
    if (cp == 0x1F5F3) return 1;                    // 🗳 ballot box is narrow too
    if (cp == 0x1F4E5) return 1;                    // 📥 inbox tray is narrow too
    if (cp == 0x1F4E8) return 1;                    // 📨 inbox is narrow too
    if (cp == 0x2B55) return 2;                     // ⭕ heavy circle renders wide
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return 2;   // emoji blocks
    return 1;
}

// Terminal display width of a UTF-8 string (cells, not bytes).
int displayWidth(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b) {
            // ANSI escape sequence — zero width. Consume the CSI
            // introducer '[' + the parameter bytes + the final byte.
            i++;
            if (i < s.size() && s[i] == '[') i++;
            while (i < s.size()) {
                unsigned char e = static_cast<unsigned char>(s[i]);
                i++;
                if (e >= 0x40 && e <= 0x7E) break;  // the final byte
            }
            continue;
        }
        uint32_t cp = 0;
        if (c < 0x80) { cp = c; i += 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; i += 1;
            if (i < s.size()) { cp = (cp << 6) | (s[i] & 0x3F); i += 1; } }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; i += 1;
            for (int k = 0; k < 2 && i < s.size(); ++k) { cp = (cp << 6) | (s[i] & 0x3F); i += 1; } }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; i += 1;
            for (int k = 0; k < 3 && i < s.size(); ++k) { cp = (cp << 6) | (s[i] & 0x3F); i += 1; } }
        else { i += 1; continue; }
        w += cpWidth(cp);
    }
    return w;
}

// Clip to a display width — the text fills the panel to its end
// (no "..." — the user wants the labels up to the edge).
std::string clip(const std::string& s, int width) {
    if (displayWidth(s) <= width) return s;
    std::string out;
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b) {
            // ANSI escape sequence — zero width, copied verbatim (so the
            // color codes survive the clipping and never count as cells).
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') j++;
            while (j < s.size()) {
                unsigned char e = static_cast<unsigned char>(s[j]);
                j++;
                if (e >= 0x40 && e <= 0x7E) break;
            }
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 1;
            if (i + 1 < s.size()) { cp = (cp << 6) | (s[i + 1] & 0x3F); len = 2; } }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 1;
            if (i + 2 < s.size()) { cp = (cp << 6) | (s[i + 1] & 0x3F); cp = (cp << 6) | (s[i + 2] & 0x3F); len = 3; } }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 1;
            if (i + 3 < s.size()) { cp = (cp << 6) | (s[i + 1] & 0x3F); cp = (cp << 6) | (s[i + 2] & 0x3F); cp = (cp << 6) | (s[i + 3] & 0x3F); len = 4; } }
        else { i += 1; continue; }
        int cw = cpWidth(cp);
        if (w + cw > width) break;
        out.append(s, i, len);
        w += cw;
        i += len;
    }
    return out;
}

// Word-wrap a string to a display width. ANSI escape sequences are
// atomic and zero-width; long words are hard-split; explicit \n forces
// a line break. Continuation lines are indented by the caller.
std::vector<std::string> wrapTextImpl(const std::string& s, int width) {
    std::vector<std::string> lines;
    std::string cur;
    std::string word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        int wd = displayWidth(word);
        if (!cur.empty() && displayWidth(cur) + 1 + wd > width) {
            lines.push_back(cur);
            cur.clear();
        }
        if (!cur.empty()) cur += " ";
        cur += word;
        word.clear();
    };
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b) {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') j++;
            while (j < s.size()) {
                unsigned char e = static_cast<unsigned char>(s[j]);
                j++;
                if (e >= 0x40 && e <= 0x7E) break;
            }
            word += s.substr(i, j - i);
            i = j;
            continue;
        }
        if (c == ' ' || c == '\n') {
            if (c == ' ' && cur.empty()) {
                // The leading spaces of a line (the code indentation)
                // ride with the first word.
                word += ' ';
                i++;
                continue;
            }
            flushWord();
            if (c == '\n') {
                // No empty rows from double newlines (\n\n in quoted
                // bodies) — a break pushes the line only when it has text.
                if (!cur.empty()) lines.push_back(cur);
                cur.clear();
            } else if (!cur.empty() && displayWidth(cur) + 1 > width) {
                lines.push_back(cur);
                cur.clear();
            }
            i++;
            continue;
        }
        size_t len = 1;
        if (c >= 0x80) {
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
        }
        word += s.substr(i, len);
        if (displayWidth(word) >= width) {  // hard-split overlong words
            flushWord();
            if (!cur.empty()) {
                lines.push_back(cur);
                cur.clear();
            }
        }
        i += len;
    }
    flushWord();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Pad to a display width (cells, not bytes — keeps the | columns aligned
// with emoji/CJK in the rows).
std::string pad(const std::string& s, int width) {
    std::string out = clip(s, width);
    int w = displayWidth(out);
    if (w < width) out.append(static_cast<size_t>(width - w), ' ');
    // Never leave a color open at the panel edge (clipped rows).
    if (out.find('') != std::string::npos) out += "[0m";
    return out;
}

std::string repeat(char c, size_t n) {
    return std::string(n, c);
}

// Highlight @mentions in a body: the @token (up to the next space) gets a
// cyan background-ish colour so pings stand out.
std::string highlightMentions(const std::string& body) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        if (body[i] == '@' && (i == 0 || body[i - 1] == ' ')) {
            size_t j = i + 1;
            while (j < body.size() && body[j] != ' ' && body[j] != '\n' &&
                   body[j] != ':' && body[j] != ',') j++;
            out += "\x1b[36m" + body.substr(i, j - i) + "\x1b[0m";
            i = j;
        } else {
            out += body[i];
            i++;
        }
    }
    return out;
}

// The matrix::Event's plain text body (newlines COLLAPSED to spaces).
std::string eventBodyImpl(const matrix::Event& ev) {
    if (ev.content.is_object()) {
        auto it = ev.content.find("body");
        if (it != ev.content.end() && it->is_string()) {
            std::string b = it->get<std::string>();
            // collapse newlines for a single-line row
            std::string one;
            for (char c : b) {
                if (c == '\n') {
                    if (!one.empty() && one.back() != ' ') one += ' ';
                } else {
                    one += c;
                }
            }
            return one;
        }
    }
    return "";
}

// Thread root event id (content.m.relates_to.m.thread.event_id) — "" if none.
std::string eventThreadRoot(const matrix::Event& ev) {
    if (!ev.content.is_object()) return "";
    auto rel = ev.content.find("m.relates_to");
    if (rel == ev.content.end() || !rel->is_object()) return "";
    // New format: m.relates_to.m.thread.event_id
    auto thr = rel->find("m.thread");
    if (thr != rel->end() && thr->is_object()) {
        auto eid = thr->find("event_id");
        if (eid != thr->end() && eid->is_string()) {
            return eid->get<std::string>();
        }
    }
    // Older format: m.relates_to.{rel_type: "m.thread", event_id}
    auto rt = rel->find("rel_type");
    if (rt != rel->end() && rt->is_string() &&
        rt->get<std::string>() == "m.thread") {
        auto eid = rel->find("event_id");
        if (eid != rel->end() && eid->is_string()) {
            return eid->get<std::string>();
        }
    }
    return "";
}

// Reply-to event id (content.m.relates_to.m.in_reply_to.event_id) — "" if none.
std::string eventReplyTo(const matrix::Event& ev) {
    if (!ev.content.is_object()) return "";
    auto rel = ev.content.find("m.relates_to");
    if (rel == ev.content.end() || !rel->is_object()) return "";
    auto ir = rel->find("m.in_reply_to");
    if (ir != rel->end() && ir->is_object()) {
        auto eid = ir->find("event_id");
        if (eid != ir->end() && eid->is_string()) {
            return eid->get<std::string>();
        }
    }
    return "";
}

// Number of thread ROOTS in a room (events that have >=1 m.thread reply).
int roomThreadCount(db::Database* db, const std::string& roomId) {
    if (!db) return 0;
    auto events = db->getEvents(roomId, 200);
    int roots = 0;
    for (const auto& ev : events) {
        for (const auto& ev2 : events) {
            if (eventThreadRoot(ev2) == ev.event_id) {
                roots++;
                break;
            }
        }
    }
    return roots;
}

// "YYYY-MM-DD" -> unix ms (UTC noon to dodge timezone edges), -1 on error.
int64_t parseDayMsImpl(const std::string& s) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -1;
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
    t.tm_hour = 12;
    auto tt = std::mktime(&t);
    return tt < 0 ? -1 : static_cast<int64_t>(tt) * 1000;
}

// Short preview of another event's body (for reply/thread indentation).

// The raw body with newlines preserved (for multiline rendering).
std::string eventBodyRaw(const matrix::Event& ev) {
    if (ev.content.is_object()) {
        auto it = ev.content.find("body");
        if (it != ev.content.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return "";
}

std::string roomDisplayNameImpl(const nlohmann::json& r) {
    std::string id = r.value("room_id", "");
    std::string name = r.value("name", "");
    if (name.empty()) name = id;
    return name;
}

// Message count comes from the DB (listRooms json carries no count).
int roomMessageCount(db::Database* db, const std::string& roomId) {
    return db ? db->getEventCount(roomId) : 0;
}

std::string senderShortImpl(const std::string& sender) {
    std::string s = sender;
    if (!s.empty() && s[0] == '@') {
        auto colon = s.find(':');
        if (colon != std::string::npos) s = s.substr(1, colon - 1);
        else s = s.substr(1);
    }
    return s;
}

std::string eventPreview(db::Database* db, const std::string& roomId,
                         const std::string& eventId) {
    matrix::Event ev;
    if (!db || eventId.empty() || !db->getEventById(eventId, ev)) return "";
    std::string b = eventBodyImpl(ev);
    if (b.empty()) return "";
    return senderShortImpl(ev.sender) + ": " + clip(b, 24);
}


// The time of the last event in a room, for the room-list rows: today's
// events show HH:MM (HH:MM:SS with the "time full" setting), older ones
// show the date as MM-DD (Element Classic style).
// The newest actual MESSAGE of a room (the shared preview/timestamp
// source); defined below, used by the roomLastTime first.
std::string roomLastTime(db::Database* db, const std::string& roomId,
                          bool seconds, bool clock12h) {
    if (!db) return "";
    matrix::Event lastEv = roomLastEvent(db, roomId);
    if (lastEv.event_id.empty()) return "";
    std::time_t t = static_cast<std::time_t>(lastEv.origin_server_ts / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{};
    localtime_r(&nowT, &nowTm);
    char buf[16];
    if (tm.tm_year == nowTm.tm_year && tm.tm_yday == nowTm.tm_yday) {
        if (clock12h) {
            int h12 = tm.tm_hour % 12;
            if (h12 == 0) h12 = 12;
            const char* ap = tm.tm_hour < 12 ? "AM" : "PM";
            if (seconds) {
                std::snprintf(buf, sizeof(buf), "%d:%02d:%02d %s",
                              h12, tm.tm_min, tm.tm_sec, ap);
            } else {
                std::snprintf(buf, sizeof(buf), "%d:%02d %s", h12, tm.tm_min, ap);
            }
        } else {
            std::snprintf(buf, sizeof(buf), seconds ? "%02d:%02d:%02d" : "%02d:%02d",
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
        }
    } else {
        std::snprintf(buf, sizeof(buf), "%02d-%02d", tm.tm_mon + 1, tm.tm_mday);
    }
    return buf;
}

// The timestamp of a room's last event (for activity sorting; 0 = none).
int64_t roomLastTs(db::Database* db, const std::string& roomId) {
    if (!db) return 0;
    auto evs = db->getEvents(roomId, 1);
    if (evs.empty()) return 0;
    return evs.front().origin_server_ts;
}



// Render links in a body: matrix.to permalinks become Element-style pills
// ("📎 roomname · user: preview"), every other http(s) URL is shown in blue.
std::string renderPermalinks(const std::string& body,
                             const std::vector<nlohmann::json>& rooms,
                             db::Database* db) {
    std::string out;
    size_t pos = 0;
    const std::string marker = "matrix.to/#/";
    while (true) {
        size_t hit = body.find(marker, pos);
        if (hit == std::string::npos) {
            out += body.substr(pos);
            break;
        }
        // Strip the scheme (https://) right before the marker.
        size_t linkStart = hit;
        if (linkStart >= 8 && body.compare(linkStart - 8, 8, "https://") == 0) {
            linkStart -= 8;
        } else if (linkStart >= 7 && body.compare(linkStart - 7, 7, "http://") == 0) {
            linkStart -= 7;
        }
        out += body.substr(pos, linkStart - pos);
        // Extract the room part (up to '/' or the end) and the event part.
        size_t after = hit + marker.size();
        size_t slash = body.find('/', after);
        std::string roomPart = body.substr(after, slash == std::string::npos
                                                   ? std::string::npos
                                                   : slash - after);
        std::string evPart = slash == std::string::npos ? "" : body.substr(slash + 1);
        // The full link span (the URL). Some clients wrap it in < > or ().
        size_t spanEnd = slash == std::string::npos ? body.size() : slash + 1 + evPart.size();
        // Build the pill: room name + sender + preview for event links.
        std::string roomName = roomPart;
        for (const auto& r : rooms) {
            std::string id = r.value("room_id", "");
            if (id == roomPart) {
                roomName = r.value("name", roomPart);
                break;
            }
        }
        std::string pill = "\x1b[34m\xf0\x9f\x93\x8e " + clip(roomName, 20);  // blue 📎
        if (!evPart.empty() && db) {
            matrix::Event ev;
            if (db->getEventById(evPart, ev)) {
                std::string preview = eventBodyImpl(ev);
                pill += " \u00b7 " + senderShortImpl(ev.sender) + ": " + clip(preview, 24);
            } else {
                pill += " \u00b7 (event)";
            }
        }
        out += pill + "\x1b[0m";
        pos = spanEnd;
        if (pos > body.size()) break;
    }
    return out;
}


// Blue-highlight the plain http(s):// URLs in a text span (the matrix.to
// ones are handled by renderPermalinks).
std::string highlightUrls(const std::string& text) {
    std::string out;
    size_t pos = 0;
    const char* schemes[] = {"http://", "https://"};
    while (pos < text.size()) {
        size_t best = std::string::npos;
        for (auto* s : schemes) {
            size_t hit = text.find(s, pos);
            if (hit != std::string::npos && (best == std::string::npos || hit < best)) {
                best = hit;
            }
        }
        if (best == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, best - pos);
        size_t j = best;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n' &&
               text[j] != ',' && text[j] != ')' && text[j] != ']') j++;
        out += "\x1b[34m" + text.substr(best, j - best) + "\x1b[0m";
        pos = j;
    }
    return out;
}

// The last message of a room as a preview row ("alice: Welcome!") like
// Element's room list. Message events only; joins/lefts show a short
// summary; rooms with nothing at all get an empty string. Permalinks in
// the body render as pills, so a linked message's sender is visible too.
// The newest actual MESSAGE of a room — pins/state events (the newest
// entries) must not swallow the preview NOR the timestamp.
matrix::Event roomLastEvent(db::Database* db,
                            const std::string& roomId) {
    matrix::Event out;
    if (!db) return out;
    auto evs = db->getEvents(roomId, 30);
    for (const auto& e : evs) {
        if (e.type == "m.room.message" || e.type == "m.sticker") return e;
    }
    if (!evs.empty()) return evs.front();
    return out;
}

std::string drawFrameImpl(const UiState& st) {
    int W = terminalWidthImpl();
    // Few members: the user list goes horizontal (one row across the top
    // of the chat) and the right panel is freed - auto mode, or forced.
    bool horizMembers = !st.mobile && st.rightPanel == 0 &&
        (st.membersMode == 1 ||
         (st.membersMode == 0 && st.members.size() <= 4));
    int leftW = st.leftPanelW >= 0 ? st.leftPanelW : std::max(22, W / 5);
    int rightW = st.rightPanelW >= 0 ? st.rightPanelW : std::max(16, W / 6);
    if (st.leftPanelW == 0) leftW = 0;
    if (st.rightPanelW == 0) rightW = 0;
    if (st.autoPanels) {
        // Size the panels to the content so the screen is filled: the
        // room panel fits the longest room row plus a preview budget, the
        // member panel fits the longest member name.
        // The rooms list is sized to the LONGEST ACTUAL row (name, count,
        // thread marker and the last-message preview), so no column space
        // is wasted — the chat keeps the rest.
        int longestRoom = 0;
        for (const auto& r : st.rooms) {
            if (r.value("is_space", false)) continue;
            std::string rid = r.value("room_id", "");
            std::string nm = roomDisplayNameImpl(r);
            if (r.value("is_direct", false)) nm = "  " + nm;
            int w = displayWidth(" " + nm + " ("
                                 + std::to_string(roomMessageCount(st.db, rid)) + ")");
            auto invIt = st.inviteByRoom.find(rid);
            if (invIt != st.inviteByRoom.end() && invIt->second.ts > 0) {
                // Invited rooms render "📨 <when>" instead of the
                // last-message preview.
                w += displayWidth(" 📨 ") + displayWidth(relativeTime(invIt->second.ts));
            } else {
                std::string last = roomLastMsg(st.db, rid, st.rooms);
                if (!last.empty()) w += 3 + displayWidth(last);  // " · preview"
            }
            if (st.db->getSetting("threads_off", "0") == "0" &&
                roomThreadCount(st.db, rid) > 0) w += 4;  // " 🧵N"
            if (w > longestRoom) longestRoom = w;
        }
        leftW = std::max(24, std::min(56, longestRoom + 2));
        if (horizMembers) {
            rightW = 0;
        } else {
            int longestMember = 0;
            int fullMember = 0;
            for (const auto& mem : st.members) {
                int w = displayWidth(memberRowStr(st, mem));
                if (w > longestMember) longestMember = w;
                int wf = displayWidth(memberRowStr(st, mem, true));
                if (wf > fullMember) fullMember = wf;
            }
            // The full matrix ids fit when they stay inside the clamp.
            if (fullMember + 3 <= 30) longestMember = fullMember;
            // The thread list at the bottom may need more room — but the
            // panel stays capped so the chat never gets squeezed out.
            if (st.showThreadsBottom) {
                auto thr = roomThreadList(st.db, st.currentRoomId, 30, st.showIds);
                for (const auto& t : thr) {
                    int w = displayWidth(t);
                    if (w > longestMember) longestMember = w;
                }
            }
            int rightMax = std::min(40, std::max(24, W / 3));
            rightW = std::max(10, std::min(rightMax, longestMember + 3));
        }
    }
    // Keep the chat usable: the panels never squeeze the center below
    // ~30 columns — the rooms list gives way first, then the members.
    int minCenter = 30;
    if (W - leftW - rightW - 2 < minCenter) {
        leftW = std::max(24, W - rightW - 2 - minCenter);
    }
    if (W - leftW - rightW - 2 < minCenter) {
        rightW = std::max(10, W - leftW - 2 - minCenter);
    }
    // The chat caps its width: on very wide terminals the leftover stays
    // as a right margin instead of an empty strip inside the message area.
    int centerW = std::max(20, W - leftW - rightW - 2);
    int centerCap = st.mobile ? W - 2 : 120;
    if (centerW > centerCap) centerW = centerCap;

    std::string roomName = "No room selected";
    std::string e2eeMark;  // the lock for the open room
    for (const auto& r : st.rooms) {
        if (r.value("room_id", "") == st.currentRoomId) {
            roomName = roomDisplayNameImpl(r);
            if (r.value("is_encrypted", false)) {
                e2eeMark = (st.showEmoji ? " 🔒 " : " [E2EE] ");
            }
            break;
        }
    }

    // Header
    std::string out;
    // The unread count since the last-read marker — the Element-style
    // "· N new" hint in the room header (visible regardless of the
    // viewport scroll).
    int newSinceRead = 0;
    if (!st.readMarker.empty()) {
        bool past = false;
        for (const auto& ev : st.messages) {
            if (!past) {
                if (ev.event_id == st.readMarker) past = true;
                continue;
            }
            if (ev.type == "m.room.message" || ev.type == "m.sticker")
                newSinceRead++;
        }
    }
    std::string header = " " + roomName + e2eeMark;
    if (newSinceRead > 0) header += " · " + std::to_string(newSinceRead) + " new";
    header += " ";
    int headerFill = W - static_cast<int>(header.size()) - 1;
    if (headerFill < 0) headerFill = 0;
    out += header + repeat('=', headerFill) + "\n";

    // Panel header row
    // The room's topic (short) under the header line.
    std::string topic;
    for (const auto& r : st.rooms) {
        if (r.value("room_id", "") == st.currentRoomId) {
            topic = r.value("topic", "");
            break;
        }
    }
    if (!topic.empty()) {
        out += "  " + clip(topic, W - 2) + "\n";
    }
    // Pinned messages (the "pin" command inserts m.room.pinned_events):
    // a 📌 line with the preview of the last pinned event.
    if (!st.currentRoomId.empty()) {
        auto evs = st.db->getEvents(st.currentRoomId, 300);
        for (const auto& ev : evs) {
            if (ev.type != "m.room.pinned_events") continue;
            auto pinned = ev.content.find("pinned");
            if (pinned == ev.content.end() || !pinned->is_array() ||
                pinned->empty()) continue;
            const std::string& pid = (*pinned)[0].get<std::string>();
            matrix::Event pev;
            if (st.db->getEventById(pid, pev)) {
                std::string pv = eventBodyImpl(pev);
                out += "  \xf0\x9f\x93\x8c " + clip(pv, W - 4) + "\n";
            }
            break;
        }
    }
    // Send permission (like Element): @you's level vs the room default.
    {
        int myLevel = 0;
        auto me = st.accountLabel == "demo (offline)" ? "@you" : "@" + st.accountLabel;
        auto it = st.powerLevels.find(me);
        if (it != st.powerLevels.end()) myLevel = it->second;
        if (myLevel < st.eventsDefault && !st.currentRoomId.empty()) {
            out += "  \x1b[31m[read-only: level " + std::to_string(st.eventsDefault)
                 + " required to send (you: " + std::to_string(myLevel) + ")]\x1b[0m\n";
        }
    }
    // Smartphone top bar: the logged-in account, the open-invite count and
    // where we are (the space name, or "all rooms" when everything shows).
    if (st.mobile) {
        std::string where = "all rooms";
        if (!st.activeSpace.empty()) {
            for (const auto& r : st.rooms) {
                if (r.value("room_id", "") == st.activeSpace) {
                    where = r.value("name", "space");
                    break;
                }
            }
        }
        std::string top = st.accountLabel;
        if (st.invites > 0) {
            top += " \xf0\x9f\x93\xa5 " + std::to_string(st.invites) + " invites";
        }
        top += " \xc2\xb7 " + where;
        out += "\x1b[90m  " + clip(top, W - 2) + "\x1b[0m\n";
        // The space strip (Element Classic): one tap selects the space.
        if (st.mobileTab == 0) {
            std::string strip = " [all] ";
            for (const auto& r : st.rooms) {
                if (!r.value("is_space", false)) continue;
                bool active = r.value("room_id", "") == st.activeSpace;
                std::string label = clip(r.value("name", "?"), 12);
                if (active) {
                    strip += "\x1b[7m" + label + "\x1b[0m ";
                } else {
                    strip += label + " ";
                }
            }
            out += "\x1b[36m" + clip(strip, W - 2) + "\x1b[0m\n";
        }
    }
    std::string leftHeader = st.accountLabel + " — Rooms";
    if (st.invites > 0 && st.showInvites) {
        leftHeader += (st.showEmoji ? " 📥 " : " [inv] ") + std::to_string(st.invites)
                    + (st.showEmoji && st.showInvitesLegend ? " (invites)" : "");
    }
    std::string headRoom = " " + roomName;
    if (static_cast<int>(headRoom.size()) > centerW - 1) headRoom = headRoom.substr(0, centerW - 1);
    const char* PIPE = "\x1b[90m";  // dim grey for the panel pipes
    const char* X = "\x1b[0m";
    if (!st.mobile) {
        const char* rightTitle = st.rightPanel == 2 ? "Thread"
                                 : st.rightPanel == 1 ? "Threads"
                                 : st.rightPanel == 3 ? "All threads"
                                 : st.rightPanel == 4 ? "Agent"
                                 : "Members";
        out += pad(leftHeader, static_cast<size_t>(leftW)) + PIPE + "|" + X
             + pad(headRoom, static_cast<size_t>(centerW)) + PIPE + "|" + X
             + std::string(rightTitle) + "\n";
        out += PIPE + repeat('-', leftW) + "+" + repeat('-', centerW) + "+"
             + repeat('-', std::max(0, rightW - 1)) + X + "\n";
    }

    // Body rows: fill the terminal height or default to 24 rows.
    // The "rows <n>" setting (0 = auto) overrides the height; the room
    // list then scrolls inside the window (up/down/top/bottom/scroll n).
    int rows = 24;
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 6) {
        rows = static_cast<int>(ws.ws_row) - 5;
    }
#endif
    if (st.limitRows > 0) rows = st.limitRows;

    // Room filter (find <q> / space <q>): the left panel shows only the
    // matching rooms; the center/members keep their own (unfiltered) view.
    // Space rooms are never listed as rooms; with a space selected only
    // that space's children show.
    // Hidden rooms: hide for hiddenSeconds (default 12s), then return.
    // Computed locally — drawFrame takes a const state.
    std::unordered_set<std::string> hiddenNow = st.hiddenRooms;
    if (!st.hiddenUntil.empty()) {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (const auto& [rid, until] : st.hiddenUntil) {
            if (now >= until) hiddenNow.erase(rid);
        }
    }
    std::vector<const nlohmann::json*> visible;
    for (const auto& r : st.rooms) {
        if (r.value("is_space", false)) continue;
        if (hiddenNow.count(r.value("room_id", ""))) continue;
        if (!st.activeSpace.empty() &&
            r.value("space", "") != st.activeSpace) continue;
        if (st.roomFilter.empty()) {
            visible.push_back(&r);
        } else {
            std::string id = r.value("room_id", "");
            std::string name = r.value("name", "");
            if (name.find(st.roomFilter) != std::string::npos ||
                id.find(st.roomFilter) != std::string::npos) {
                visible.push_back(&r);
            }
        }
    }
    // Pre-build the center panel rows: one per message, with date separators
    // ("── Today ──") and the message time, so the viewport scrolls over

    int scroll = std::max(0, st.scroll);
    return drawFrameChatImpl(st, centerW, horizMembers, W, leftW, rightW,
                             scroll, rows, PIPE, X, std::move(out), visible,
                             roomName);
}

std::string drawFrame(const UiState& st) { return drawFrameImpl(st); }
void loadRoomIntoState(UiState& st, const std::string& query) {
    loadRoomIntoStateImpl(st, query);
}
int terminalWidth() { return terminalWidthImpl(); }
std::string roomDisplayName(const nlohmann::json& r) { return roomDisplayNameImpl(r); }
std::string senderShort(const std::string& sender) { return senderShortImpl(sender); }
int contentRows(const UiState& st) { return contentRowsImpl(st); }

// The public wrapper over the anonymous-namespace renderer — the llm CLI
// shares the same ANSI markdown rendering as the chat view.

std::vector<std::string> wrapText(const std::string& s, int width) {
    return wrapTextImpl(s, width);
}
int64_t parseDayMs(const std::string& s) { return parseDayMsImpl(s); }
std::string eventBody(const matrix::Event& ev) { return eventBodyImpl(ev); }
int centerRowIndexOf(const UiState& st, const std::string& eventId) {
    return centerRowIndexOfImpl(st, eventId);
}

std::string renderMarkdownAnsi(const std::string& body) {
    return renderMarkdownBody(body);
}


int cmdAttachFile(const cli::Args& args);

// ---- Local (offline) file message ----
// Used by the demo REPL and the ui REPL when no session is available:
// inserts an m.file event into the local DB (no upload, no account).
int uiInsertLocalFile(db::Database& dbi, const std::string& roomId,
                      const std::string& path, const std::string& caption,
                      const std::string& threadRoot) {
    std::string fn = path;
    auto slash = fn.find_last_of('/');
    if (slash != std::string::npos) fn = fn.substr(slash + 1);
    std::uintmax_t size = 0;
    try {
        size = std::filesystem::file_size(path);
    } catch (...) {}
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    matrixcli::matrix::Event ev;
    ev.event_id = "$ui_" + std::to_string(ts);
    ev.room_id = roomId;
    ev.sender = "@you:demo.local";
    ev.type = "m.room.message";
    nlohmann::json content = {
        {"msgtype", "m.file"},
        {"body", caption.empty() ? fn : caption},
        {"filename", fn},
        {"url", "mxc://demo.local/local-file"},
        {"info", {{"size", size}, {"mimetype", "application/octet-stream"}}},
    };
    if (!threadRoot.empty()) {
        content["m.relates_to"]["rel_type"] = "m.thread";
        content["m.relates_to"]["event_id"] = threadRoot;
    }
    ev.content = content;
    ev.origin_server_ts = ts;
    dbi.insertEvent(ev);
    return 0;
}

} // namespace matrixcli

// Register the ASCII UI command (via the extensible registry). Defined at
// the GLOBAL scope, matching the other register* functions.
void registerAsciiUiCommand() {
    matrixcli::CommandRegistry::instance().registerCli(
        "about", matrixcli::cmdAbout,
        "Show the about screen (logo, version, proxy, account)");
    matrixcli::CommandRegistry::instance().registerCli(
        "ui", matrixcli::cmdAsciiUi,
        "ASCII-drawn client interface (rooms | chat | members)");
    matrixcli::CommandRegistry::instance().registerCli(
        "attach", matrixcli::cmdAttachFile,
        "Send a file: attach <room> <file> [--caption text]");
    matrixcli::CommandRegistry::instance().registerCli(
        "send-file", matrixcli::cmdAttachFile,
        "Send a file (alias of attach)");
} // namespace matrixcli

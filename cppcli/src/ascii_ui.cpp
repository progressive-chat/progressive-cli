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

namespace matrixcli {

namespace {

// ---- small terminal/string helpers ----

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
std::map<uint32_t, int> g_widthOverrides;

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
    // pipes. Only the real emoji blocks and CJK are wide.
    if (cp == 0x1F451 || cp == 0x1F6E1) return 1;   // 👑 🛡 power badges are narrow
    if (cp == 0x1F5F3) return 1;                    // 🗳 ballot box is narrow too
    if (cp == 0x2B55) return 2;                     // ⭕ heavy circle renders wide
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return 2;    // emoji blocks
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
static matrix::Event roomLastEvent(db::Database* db,
                                   const std::string& roomId);

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
static matrix::Event roomLastEvent(db::Database* db,
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

std::string roomLastMsg(db::Database* db, const std::string& roomId,
                        const std::vector<nlohmann::json>& rooms) {
    if (!db) return "";
    // The newest MESSAGE — pins/state events (the newest entries) must not
    // swallow the preview. (By value: the pointer into the temporary
    // getEvents vector was a dangling-reference crash.)
    matrix::Event ev = roomLastEvent(db, roomId);
    if (ev.event_id.empty()) return "";
    std::string preview;
    if (ev.type == "m.room.message" || ev.type == "m.sticker") {
        std::string body = eventBodyImpl(ev);
        std::string mt;
        auto mtIt = ev.content.find("msgtype");
        if (mtIt != ev.content.end() && mtIt->is_string()) mt = mtIt->get<std::string>();
        if (mt == "m.file") preview = "📄 " + body;
        else if (mt == "m.audio") preview = "🎵 " + body;
        else if (mt == "m.image") preview = "🖼 " + body;
        else if (mt == "m.poll.start") {
            auto q = ev.content.find("question");
            if (q != ev.content.end() && q->is_object()) {
                auto t = q->find("text");
                if (t != q->end() && t->is_string()) preview = t->get<std::string>();
            }
            preview = "⭕ poll: " + preview;
        }
        else if (ev.type == "m.sticker") preview = "👻 " + body;
        else preview = highlightUrls(renderPermalinks(body, rooms, db));
    } else if (ev.type == "m.room.member") {
        std::string m = ev.content.value("membership", "");
        preview = (m == "leave" || m == "ban") ? "❌ left"
                                               : "✓ joined";
    }
    if (preview.empty()) return "";
    return senderShortImpl(ev.sender) + ": " + preview;
}


// ---- the frame ----


// The row index of an event inside the chat timeline (day separators
// included) — mirrors the centerRows builder in drawFrame.
int centerRowIndexOfImpl(const UiState& st, const std::string& eventId) {
    int row = 0;
    int64_t prevDay = -1;
    for (const auto& ev : st.messages) {
        int64_t day = ev.origin_server_ts / 86400000;
        if (day != prevDay) { row++; prevDay = day; }
        if (ev.event_id == eventId) return row;
        // Rows this message occupies: multiline bodies and the reply
        // chain are split into one row per line.
        int nl = 0;
        std::string body = eventBodyRaw(ev);
        for (char ch : body) if (ch == '\n') nl++;
        std::string rep = eventReplyTo(ev);
        if (!rep.empty()) {
            std::string cur = rep;
            for (int lvl = 0; lvl < 3; ++lvl) {
                matrix::Event prev;
                if (!st.db->getEventById(cur, prev)) break;
                if (eventBodyImpl(prev).empty()) break;
                nl++;
                auto rel = prev.content.find("m.relates_to");
                if (rel == prev.content.end() || !rel->is_object()) break;
                auto ir = rel->find("m.in_reply_to");
                if (ir == rel->end() || !ir->is_object()) break;
                auto eid = ir->find("event_id");
                if (eid == ir->end() || !eid->is_string()) break;
                cur = eid->get<std::string>();
            }
        }
        row += nl + 1;
    }
    return -1;
}

// Element-style room list: most recently active rooms first.
void sortRoomsByActivity(UiState& st) {
    // Starred (anchored) rooms are pinned to the top, like Element's
    // Favourite; the rest sorts by last activity.
    std::stable_sort(st.rooms.begin(), st.rooms.end(),
        [&st](const nlohmann::json& a, const nlohmann::json& b) {
            std::string ia = a.value("room_id", "");
            std::string ib = b.value("room_id", "");
            bool sa = st.starredRooms.count(ia) != 0;
            bool sb = st.starredRooms.count(ib) != 0;
            if (sa != sb) return sa;
            int64_t ta = roomLastTs(st.db, ia);
            int64_t tb = roomLastTs(st.db, ib);
            if (ta != tb) return ta > tb;
            return ia < ib;
        });
}


// Human-readable proxy state from the core's global config.
std::string proxyLabelText() {
    auto p = progressive::desktop::getGlobalProxy();
    if (!p.enabled || p.host.empty()) return "off";
    std::string type = "socks5h";
    if (p.type == progressive::desktop::ProxyConfig::Type::Socks5) type = "socks5";
    else if (p.type == progressive::desktop::ProxyConfig::Type::Http) type = "http";
    return "on (" + type + " " + p.host + ":" + std::to_string(p.port) + ")";
}

// Total content rows across the three panels (the scrollable height).
int contentRowsImpl(const UiState& st) {
    int n = static_cast<int>(st.rooms.size());
    n = std::max(n, static_cast<int>(st.messages.size()));
    n = std::max(n, static_cast<int>(st.members.size()));
    return n;
}

// Shortest unique prefixes of the given names (e.g. ["alice","bob","charlie"]
// -> ["a","b","ch"]). Falls back to the full name when everything collides.
std::map<std::string, std::string> minimalUniqueNames(
    const std::vector<std::string>& members) {
    std::map<std::string, std::string> out;
    for (const auto& m : members) {
        std::string name = senderShortImpl(m);
        if (name.empty()) continue;
        // The shortest prefix not shared with any other member.
        std::string prefix;
        for (size_t len = 1; len <= name.size(); ++len) {
            prefix = name.substr(0, len);
            bool unique = true;
            for (const auto& o : members) {
                if (o == m) continue;
                if (senderShortImpl(o).compare(0, prefix.size(), prefix) == 0) {
                    unique = false;
                    break;
                }
            }
            if (unique) break;
        }
        out[m] = prefix;
    }
    return out;
}


std::map<std::string, std::string> minimalUniqueNames(
    const std::vector<std::string>& members);

// The ids of the room's pinned events (from the latest m.room.pinned_events).
std::unordered_set<std::string> pinnedIds(db::Database* db,
                                           const std::string& roomId) {
    std::unordered_set<std::string> out;
    if (!db || roomId.empty()) return out;
    auto evs = db->getEvents(roomId, 300);
    for (const auto& ev : evs) {
        if (ev.type != "m.room.pinned_events") continue;
        auto pinned = ev.content.find("pinned");
        if (pinned == ev.content.end() || !pinned->is_array()) continue;
        out.clear();
        for (const auto& id : *pinned) {
            if (id.is_string()) out.insert(id.get<std::string>());
        }
        break;
    }
    return out;
}

void loadRoomIntoStateImpl(UiState& st, const std::string& query) {
    st.currentRoomId.clear();
    // Accept the id ("!room:server"), the full alias ("#alias:server") or
    // the short alias/name ("#alias", "alias").
    std::string q = query;
    if (q.size() > 1 && q[0] == '#') {
        auto colon = q.find(':');
        if (colon != std::string::npos) q = q.substr(0, colon);  // #alias:server -> #alias
    }
    for (const auto& r : st.rooms) {
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        bool byId = !q.empty() && q[0] == '!' && id == q;
        bool byName = !q.empty() && (name == q ||
                                     name.find(q) == 0 ||
                                     name.find(q) != std::string::npos);
        if (byId || byName) {
            st.currentRoomId = id;
            break;
        }
    }
    if (st.currentRoomId.empty() && !st.rooms.empty() && query.empty()) {
        st.currentRoomId = st.rooms.front().value("room_id", "");
    }
    if (st.currentRoomId.empty()) {
        st.messages.clear();
        st.members.clear();
        return;
    }
    st.messages = st.db->getEvents(st.currentRoomId, st.limit);
    std::reverse(st.messages.begin(), st.messages.end());  // newest last
    st.readMarker = st.db->getReadMarker(st.currentRoomId);
    st.pinned = pinnedIds(st.db, st.currentRoomId);
    st.scroll = 0;  // clamped to the bottom in drawFrame
    st.members.clear();
    st.powerLevels.clear();
    st.eventsDefault = 0;
    st.redactedIds.clear();
    st.memberNames.clear();
    // Display names come from the member events — scan the whole history
    // (the chat window may not reach the joins).
    auto allEvs = st.db->getEvents(st.currentRoomId, 2000);
    for (const auto& ev : allEvs) {
        if (st.memberNames.count(ev.sender)) continue;  // newest wins
        if (ev.type == "m.room.member" && ev.content.is_object()) {
            auto dn = ev.content.find("displayname");
            if (dn != ev.content.end() && dn->is_string() &&
                !dn->get<std::string>().empty()) {
                st.memberNames[ev.sender] = dn->get<std::string>();
            }
        }
    }
    for (const auto& ev : st.messages) {
        if (std::find(st.members.begin(), st.members.end(), ev.sender) ==
            st.members.end()) {
            st.members.push_back(ev.sender);
        }
        if (ev.type == "m.room.power_levels" && ev.content.is_object()) {
            st.powerLevelsEvent = ev.content;
            auto users = ev.content.find("users");
            if (users != ev.content.end() && users->is_object()) {
                for (auto& [uid, lvl] : users->items()) {
                    if (lvl.is_number()) st.powerLevels[uid] = lvl.get<int>();
                }
            }
        }
            auto ed = ev.content.find("events_default");
            if (ed != ev.content.end() && ed->is_number()) {
                st.eventsDefault = ed->get<int>();
            }
        if (!ev.redacts.empty()) st.redactedIds.insert(ev.redacts);
    }
    // Members whose localparts collide (two "@alice" from different
    // servers) get the full mxid in the chat rows.
    st.nameColliders.clear();
    {
        std::map<std::string, int> counts;
        for (const auto& m : st.members) counts[senderShortImpl(m)]++;
        for (const auto& m : st.members) {
            if (counts[senderShortImpl(m)] > 1) st.nameColliders.insert(m);
        }
    }
    // Read receipts (approximation): a message is read by the members whose
    // own message comes AFTER it in the timeline.
    st.receipts.clear();
    auto abbrev = minimalUniqueNames(st.members);
    for (const auto& ev : st.messages) {
        // Deduplicate: a sender with several later messages counts once.
        std::string readers;
        std::unordered_set<std::string> seen;
        for (const auto& ev2 : st.messages) {
            if (ev2.origin_server_ts > ev.origin_server_ts &&
                ev2.sender != ev.sender && !seen.count(ev2.sender)) {
                seen.insert(ev2.sender);
                auto it = abbrev.find(ev2.sender);
                if (it != abbrev.end()) {
                    if (!readers.empty()) readers += " ";
                    readers += it->second;
                }
            }
        }
        st.receipts[ev.event_id] = readers;
    }
    // The read marker: the ✓ readers only sit on the LAST message read by
    // the user — the newest message in the room (the user has read up to
    // it). Its readers are everyone else who has read the room: all the
    // other senders, deduplicated and capped at 3 + "+N" — like Element's
    // "Seen by A, B, C" line.
    std::string me = st.accountLabel == "demo (offline)" ? "@you"
                                                         : "@" + st.accountLabel;
    std::string markerId;
    for (auto it = st.messages.rbegin(); it != st.messages.rend(); ++it) {
        if (it->sender != me) {
            markerId = it->event_id;
            break;
        }
    }
    std::string markerReaders;
    if (!markerId.empty()) {
        std::unordered_set<std::string> seen;
        std::string markerSender;
        for (const auto& ev : st.messages) {
            if (ev.event_id == markerId) { markerSender = ev.sender; break; }
        }
        for (const auto& ev : st.messages) {
            if (ev.sender == markerSender || ev.sender == me) continue;
            if (!seen.insert(ev.sender).second) continue;
            auto it = abbrev.find(ev.sender);
            if (it == abbrev.end()) continue;
            if (!markerReaders.empty()) markerReaders += " ";
            markerReaders += it->second;
        }
        // Cap: 'a b c +5' instead of the full list.
        std::string shown;
        int count = 0;
        std::istringstream rss(markerReaders);
        std::string tok;
        while (rss >> tok && count < 3) {
            if (!shown.empty()) shown += " ";
            shown += tok;
            count++;
        }
        int total = 0;
        { std::istringstream rss2(markerReaders); std::string t2; while (rss2 >> t2) total++; }
        if (count < total) shown += " +" + std::to_string(total - count);
        markerReaders = shown;
    }
    for (auto& [id, readers] : st.receipts) {
        readers = (id == markerId) ? markerReaders : "";
    }
}

// The thread rows of a room ("⤷ preview (N)") for the right panel.
// Resolve a thread root: "3" = the N-th thread of the room (the list
// order), otherwise the root id or a substring of it. Empty = not found.
std::string resolveThreadRoot(db::Database* db, const std::string& roomId,
                              const std::string& sel) {
    if (!db || sel.empty()) return "";
    auto evs = db->getEvents(roomId, 300);
    std::vector<std::string> roots;
    for (const auto& ev : evs) {
        int rc = 0;
        for (const auto& ev2 : evs) {
            if (eventThreadRoot(ev2) == ev.event_id) rc++;
        }
        if (rc > 0) roots.push_back(ev.event_id);
    }
    bool isNum = !sel.empty() &&
        std::all_of(sel.begin(), sel.end(),
                    [](unsigned char c) { return std::isdigit(c); });
    if (isNum) {
        int n = std::atoi(sel.c_str());
        if (n >= 1 && n <= static_cast<int>(roots.size())) {
            return roots[static_cast<size_t>(n - 1)];
        }
        return "";
    }
    for (const auto& r : roots) {
        if (r == sel) return r;
    }
    for (const auto& r : roots) {
        if (r.find(sel) != std::string::npos) return r;
    }
    return "";
}

std::vector<std::string> roomThreadList(db::Database* db,
                                         const std::string& roomId, int clipW,
                                         bool showIds) {
    std::vector<std::string> thr;
    if (!db || roomId.empty()) return thr;
    auto evs = db->getEvents(roomId, 300);
    for (const auto& ev : evs) {
        int rc = 0;
        const matrix::Event* last = nullptr;  // the newest reply
        for (const auto& ev2 : evs) {
            if (eventThreadRoot(ev2) == ev.event_id) {
                rc++;
                if (ev2.event_id != ev.event_id &&
                    (!last || ev2.origin_server_ts > last->origin_server_ts)) {
                    last = &ev2;
                }
            }
        }
        if (rc > 0) {
            thr.push_back("⤷ " + clip(eventBodyImpl(ev), clipW) + " ("
                          + std::to_string(rc) + ")");
            // The last reply under it: nickname + time, like Element.
            if (last) {
                std::time_t t = static_cast<std::time_t>(last->origin_server_ts / 1000);
                std::tm tm{};
                localtime_r(&t, &tm);
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
                thr.push_back("  " + senderShortImpl(last->sender) + " · "
                              + buf);
            }
            // With --ids: the thread master's full id on the next line.
            if (showIds) {
                thr.push_back("  ‹" + clip(ev.event_id, clipW - 2)
                              + "›");
            }
        }
    }
    return thr;
}

// Resolve a space query (id, #alias[:server] or name) to the space id.
std::string resolveSpace(const std::vector<nlohmann::json>& rooms,
                         const std::string& q) {
    std::string qq = q;
    if (qq.size() > 1 && qq[0] == '#') {
        auto colon = qq.find(':');
        if (colon != std::string::npos) qq = qq.substr(0, colon);
    }
    std::string localpart;
    if (!qq.empty() && qq[0] == '#') {
        localpart = qq.substr(1);
        auto colon = localpart.find(':');
        if (colon != std::string::npos) localpart = localpart.substr(0, colon);
    }
    for (const auto& r : rooms) {
        if (!r.value("is_space", false)) continue;
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        if (id == qq || name == qq ||
            (!localpart.empty() && id.find(localpart) != std::string::npos)) {
            return id;
        }
    }
    std::string ql = q;
    for (auto& ch : ql) ch = static_cast<char>(std::tolower(ch));
    for (const auto& r : rooms) {
        if (!r.value("is_space", false)) continue;
        std::string name = r.value("name", "");
        for (auto& ch : name) ch = static_cast<char>(std::tolower(ch));
        if (!ql.empty() && name.find(ql) != std::string::npos) {
            return r.value("room_id", "");
        }
    }
    return "";
}


// The via arguments for a permalink: the distinct server names of the
// room's members. limit = 0 means ALL of them (the user's choice), else
// the first N — Element sends 3 by default.
std::string viaSuffix(db::Database* db, const std::string& roomId, int limit) {
    if (!db || roomId.empty()) return "";
    auto evs = db->getEvents(roomId, 500);
    std::vector<std::string> servers;
    for (const auto& ev : evs) {
        std::string s = ev.sender;
        auto colon = s.find(':');
        if (colon == std::string::npos) continue;
        std::string domain = s.substr(colon + 1);
        if (domain.empty()) continue;
        bool seen = false;
        for (const auto& sv : servers) {
            if (sv == domain) { seen = true; break; }
        }
        if (!seen) servers.push_back(domain);
    }
    if (servers.empty()) return "";
    std::string out;
    int n = (limit <= 0) ? static_cast<int>(servers.size())
                         : std::min(limit, static_cast<int>(servers.size()));
    for (int i = 0; i < n; ++i) {
        out += (i == 0 ? "?" : "&");
        out += "via=" + servers[static_cast<size_t>(i)];
    }
    return out;
}

// One member row: presence letter (colored), power badge, name.
// Users without a presence entry (server with presence off, not yet
// fetched) show as offline [F] — never without a letter.
// The per-room display name (the "nick" setting) or the short sender.
std::string displayName(const UiState& st, const std::string& roomId,
                        const std::string& sender) {
    auto it = st.roomNicks.find(roomId + "|" + sender);
    if (it != st.roomNicks.end() && !it->second.empty()) return it->second;
    return senderShortImpl(sender);
}

// The user's custom highlight color (the "color" setting).
const char* userColorCode(const UiState& st, const std::string& sender) {
    auto it = st.userColors.find(sender);
    if (it == st.userColors.end()) return nullptr;
    const std::string& c = it->second;
    if (c == "red") return "\x1b[31m";
    if (c == "green") return "\x1b[32m";
    if (c == "yellow") return "\x1b[33m";
    if (c == "blue") return "\x1b[34m";
    if (c == "magenta") return "\x1b[35m";
    if (c == "cyan") return "\x1b[36m";
    return nullptr;
}

// The full @user:server id: real sessions carry it in the sender; the
// demo stores it in the presence map under "@short:demo.local".
std::string fullMxid(const UiState& st, const std::string& mem);

// The display name for the chat rows — a custom nick, then the member's
// displayname; members whose localparts collide (two "@alice" from
// different servers) get the full mxid instead of the ambiguous short.
std::string chatName(const UiState& st, const std::string& roomId,
                     const std::string& sender) {
    std::string nm = displayName(st, roomId, sender);
    if (nm == senderShortImpl(sender) && st.nameColliders.count(sender)) {
        nm = fullMxid(st, sender);
    }
    return nm;
}

// ---- the source highlighter + the minimal markdown ----

static const std::unordered_set<std::string>& cppKeywords() {
    static const std::unordered_set<std::string> kw = {
        "alignas","alignof","and","asm","auto","bool","break","case","catch",
        "char","class","concept","const","constexpr","consteval","constinit",
        "continue","co_await","co_return","co_yield","decltype","default",
        "delete","do","double","dynamic_cast","else","enum","explicit",
        "export","extern","false","float","for","friend","goto","if","inline",
        "int","long","mutable","namespace","new","noexcept","nullptr",
        "operator","or","private","protected","public","register",
        "reinterpret_cast","requires","return","short","signed","sizeof",
        "static","static_assert","static_cast","struct","switch","template",
        "this","thread_local","throw","true","try","typedef","typeid",
        "typename","union","unsigned","using","virtual","void","volatile",
        "wchar_t","while","xor","and_eq","bitand","bitor","compl","not",
        "not_eq","or_eq","xor_eq","include","define","ifdef","ifndef",
        "endif","pragma","elif","undef","error","warning","elifdef",
        "elifndef","override","final","nullptr_t","std",
    };
    return kw;
}

std::string highlightCodeLine(const std::string& line, bool cFamily) {
    std::string out;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (cFamily && c == '#' &&
            (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
            out += "\x1b[33m" + line.substr(i) + "\x1b[0m";
            break;
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            out += "\x1b[90m" + line.substr(i) + "\x1b[0m";
            break;
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            auto end = line.find("*/", i + 2);
            size_t e = end == std::string::npos ? line.size() : end + 2;
            out += "\x1b[90m" + line.substr(i, e - i) + "\x1b[0m";
            i = e;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            size_t j = i + 1;
            while (j < line.size() && line[j] != q) {
                if (line[j] == '\\') j++;
                j++;
            }
            if (j < line.size()) j++;
            out += "\x1b[32m" + line.substr(i, j - i) + "\x1b[0m";
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[j])) ||
                    line[j] == '.' || line[j] == '_')) j++;
            out += "\x1b[36m" + line.substr(i, j - i) + "\x1b[0m";
            i = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[j])) ||
                    line[j] == '_')) j++;
            std::string w = line.substr(i, j - i);
            if (cFamily && cppKeywords().count(w)) {
                out += "\x1b[35m" + w + "\x1b[0m";
            } else {
                out += w;
            }
            i = j;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

std::string renderMarkdownBody(const std::string& body) {
    std::string out;    // The inline decorations: `code`, **bold**, and [text](url) links.
    auto inlineMd = [](const std::string& t) -> std::string {
        std::string r;
        size_t i = 0;
        while (i < t.size()) {
            if (t[i] == '`') {
                auto end = t.find('`', i + 1);
                if (end != std::string::npos) {
                    r += "\x1b[36m" + t.substr(i + 1, end - i - 1)
                       + "\x1b[0m";
                    i = end + 1;
                    continue;
                }
            }
            if (t[i] == '*' && i + 1 < t.size() && t[i + 1] == '*') {
                auto end = t.find("**", i + 2);
                if (end != std::string::npos) {
                    r += "\x1b[1m" + t.substr(i + 2, end - i - 2)
                       + "\x1b[0m";
                    i = end + 2;
                    continue;
                }
            }
            if (t[i] == '[') {
                // The [text](url) link: the blue text + the dim URL.
                auto close = t.find("](", i + 1);
                if (close != std::string::npos && close + 1 < t.size() &&
                    t[close + 1] == '(') {
                    auto end = t.find(')', close + 2);
                    if (end != std::string::npos) {
                        r += "\x1b[34m" + t.substr(i + 1, close - i - 1)
                           + "\x1b[0m\x1b[90m("
                           + t.substr(close + 2, end - close - 2)
                           + ")\x1b[0m";
                        i = end + 1;
                        continue;
                    }
                }
            }
            r += t[i];
            i++;
        }
        return r;
    };
    // The per-line prefixes: the headers, the quotes, the bullets, the
    // ordered items, the checkboxes.
    auto linePrefix = [](const std::string& ln) -> std::string {
        std::string t = ln;
        size_t b = t.find_first_not_of(' ');
        if (b == std::string::npos) return t;
        std::string indent = t.substr(0, b);
        std::string rest = t.substr(b);
        if (rest.rfind("### ", 0) == 0) {
            return indent + "\x1b[1m" + rest.substr(4) + "\x1b[0m";
        }
        if (rest.rfind("## ", 0) == 0) {
            return indent + "\x1b[1m" + rest.substr(3) + "\x1b[0m";
        }
        if (rest.rfind("# ", 0) == 0) {
            return indent + "\x1b[1m" + rest.substr(2) + "\x1b[0m";
        }
        if (rest.rfind("> ", 0) == 0) {
            return indent + "\x1b[90m\u258f " + rest.substr(2) + "\x1b[0m";
        }
        if (rest.rfind("- [x] ", 0) == 0 || rest.rfind("- [X] ", 0) == 0) {
            return indent + "\u2611 " + rest.substr(6);
        }
        if (rest.rfind("- [ ] ", 0) == 0) {
            return indent + "\u2610 " + rest.substr(6);
        }
        if (rest.rfind("- ", 0) == 0 || rest.rfind("* ", 0) == 0 ||
            rest.rfind("+ ", 0) == 0) {
            return indent + "\u2022 " + rest.substr(2);
        }
        if (rest.size() > 2 && std::isdigit(
                static_cast<unsigned char>(rest[0])) && rest[1] == '.' &&
            rest[2] == ' ') {
            return indent + "\x1b[1m" + rest.substr(0, 2) + "\x1b[0m "
                 + rest.substr(3);
        }
        return t;
    };
    // Split into the fenced code blocks + the text lines.
    size_t i = 0;
    auto renderTextLines = [&](const std::string& text) -> std::string {
        std::string r;
        std::istringstream ls(text);
        std::string line;
        bool first = true;
        while (std::getline(ls, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!first) r += "\n";
            r += inlineMd(linePrefix(line));
            first = false;
        }
        return r;
    };
    while (i < body.size()) {
        auto fence = body.find("```", i);
        if (fence == std::string::npos) {
            out += renderTextLines(body.substr(i));
            break;
        }
        out += renderTextLines(body.substr(i, fence - i));
        auto nl = body.find('\n', fence);
        std::string lang;
        if (nl != std::string::npos) lang = body.substr(fence + 3, nl - fence - 3);
        auto start = nl == std::string::npos ? fence + 3 : nl + 1;
        auto end = body.find("```", start);
        if (end == std::string::npos) {
            out += body.substr(fence);
            break;
        }
        std::string code = body.substr(start, end - start);
        bool cFamily = lang == "cpp" || lang == "c++" || lang == "cxx" ||
                       lang == "cc" || lang == "c" || lang == "h" ||
                       lang == "hpp" || lang == "cpp26" || lang == "c++26";
        std::istringstream ls(code);
        std::string line;
        bool first = true;
        while (std::getline(ls, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!first) out += "\n";
            out += highlightCodeLine(line, cFamily);
            first = false;
        }
        i = end + 3;
    }
    return out;
}

// The "[nick] " tag with the custom color applied, for chat rows.
std::string senderTag(const UiState& st, const std::string& roomId,
                      const std::string& sender) {
    const char* uc = userColorCode(st, sender);
    std::string nm = chatName(st, roomId, sender);
    return "[" + (uc ? std::string(uc) + nm + "\x1b[0m" : nm) + "] ";
}

// The full @user:server id: real sessions carry it in the sender; the
// demo stores it in the presence map under "@short:demo.local".
std::string fullMxid(const UiState& st, const std::string& mem) {
    if (mem.find(':') != std::string::npos) return mem;
    for (const auto& [k, v] : st.presence) {
        if (k.size() > mem.size() && k.compare(0, mem.size(), mem) == 0 &&
            k[mem.size()] == ':') {
            return k;
        }
    }
    return mem;
}

std::string memberRowStr(const UiState& st, const std::string& mem,
                         bool fullIds = false) {
    std::string mx = fullIds ? fullMxid(st, mem) : senderShortImpl(mem);
    // The display name: a custom nick wins, then the member event's
    // displayname, then the mxid localpart.
    std::string nm = senderShortImpl(mem);
    auto rk = st.roomNicks.find(st.currentRoomId + "|" + mem);
    if (rk != st.roomNicks.end() && !rk->second.empty()) {
        nm = rk->second;
    } else {
        auto nit = st.memberNames.find(mem);
        if (nit != st.memberNames.end() && !nit->second.empty()) nm = nit->second;
    }
    // A display name that differs from the mxid string is highlighted.
    std::string namePart = nm;
    if (nm != senderShortImpl(mem)) namePart = "\x1b[34m" + nm + "\x1b[0m";
    std::string letter;
    auto pit = st.presence.find(mem);
    if (pit != st.presence.end() && !pit->second.empty()) {
        letter = pit->second;
    } else {
        letter = "F";  // offline by default
    }
    const char* pc = "\x1b[32m";
    if (letter == "A") pc = "\x1b[33m";
    else if (letter == "F") pc = "\x1b[31m";
    std::string m = std::string(pc) + "[" + letter + "]" + "\x1b[0m " + namePart
                  + " (" + mx + ")";
    auto pl = st.powerLevels.find(mem);
    if (pl != st.powerLevels.end()) {
        // The crown carries its own wide glyph box; the narrow shield
        // needs an explicit space so the letter isn't glued to it. The
        // CUSTOM levels (not 0/50/100) get the numeric badge.
        if (pl->second >= 100) m = "\xf0\x9f\x91\x91" + m;
        else if (pl->second >= 50) m = "\xf0\x9f\x9b\xa1 " + m;
        else if (pl->second > 0)
            m = "\x1b[90m[" + std::to_string(pl->second) + "]\x1b[0m " + m;
    }
    return m;
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
    int centerW = std::max(20, W - leftW - rightW - 2);

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
    if (st.invites > 0) {
        leftHeader += (st.showEmoji ? " 📥 " : " [inv] ") + std::to_string(st.invites);
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
    // them like a real timeline.
    std::vector<std::string> centerRows;
   
        // Horizontal members: one dim row across the top of the chat.
        if (horizMembers && !st.members.empty()) {
            std::string mrow;
            for (const auto& mem : st.members) {
                if (!mrow.empty()) mrow += " · ";
                mrow += memberRowStr(st, mem);
            }
            centerRows.push_back("[90m" + clip(mrow, centerW - 2) + "[0m");
            // The thread list follows as a second horizontal row.
            if (st.showThreadsBottom && !st.currentRoomId.empty()) {
                auto evs = st.db->getEvents(st.currentRoomId, 300);
                std::string trow;
                for (const auto& ev : evs) {
                    int rc = 0;
                    for (const auto& ev2 : evs) {
                        if (eventThreadRoot(ev2) == ev.event_id) rc++;
                    }
                    if (rc > 0) {
                        if (!trow.empty()) trow += "  ";
                        trow += "⤷ " + clip(eventBodyImpl(ev), 20) + " ("
                              + std::to_string(rc) + ")";
                    }
                }
                if (!trow.empty()) {
                    centerRows.push_back("[90m" + clip(trow, centerW - 2)
                                         + "[0m");
                }
            }
        }
 {
        auto renderRow = [&](const matrix::Event& ev) -> std::string {
            std::string center;
            std::string body = eventBodyRaw(ev);
            // Membership events render as "joined/left the room" rows.
            if (ev.type == "m.room.member" && ev.content.is_object()) {
                auto m = ev.content.find("membership");
                if (m != ev.content.end() && m->is_string()) {
                    std::string ms = m->get<std::string>();
                    std::string who = chatName(st, st.currentRoomId, ev.sender);
                    if (ms == "join") center = "[" + who + "] joined the room";
                    else if (ms == "leave") center = "[" + who + "] left the room";
                    else if (ms == "invite") {
                        std::string target = ev.state_key.empty()
                                                 ? "" : senderShortImpl(ev.state_key);
                        std::string reason = ev.content.value("reason", "");
                        center = "[" + who + "] invited "
                               + (target.empty() ? "someone" : target);
                        if (!reason.empty() &&
                            st.db->getSetting("invreason_chat", "1") != "0") {
                            center += " — " + reason;
                        }
                    }
                    else if (ms == "ban") center = "[" + who + "] was banned";
                    else center = "[" + who + "] membership: " + ms;
                }
                if (center.empty()) center = "[" + senderShortImpl(ev.sender) + "] (member event)";
                return center;
            }
            // Extra content types: polls, stickers, audio, video, images.
            if (ev.type == "m.room.message" && ev.content.is_object()) {
                auto mtIt = ev.content.find("msgtype");
                std::string mt = mtIt != ev.content.end() && mtIt->is_string()
                                     ? mtIt->get<std::string>() : "";
                if (mt == "m.poll.start") {
                    auto q = ev.content.find("question");
                    std::string qtext;
                    if (q != ev.content.end() && q->is_object()) {
                        auto t = q->find("text");
                        if (t != q->end() && t->is_string()) qtext = t->get<std::string>();
                    }
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u2b55 poll: " : "[poll] ")
                           + (qtext.empty() ? "?" : qtext);
                } else if (mt == "m.poll.response") {
                    // A poll vote — the event has no body, render it as a
                    // vote row instead of an empty "[alice]".
                    auto rel = ev.content.find("m.relates_to");
                    std::string target;
                    if (rel != ev.content.end() && rel->is_object()) {
                        target = rel->value("event_id", "");
                    }
                    // The vote row uses plain text — the 🗳 glyph renders
                    // one cell in the phone terminal and shifted the pipes.
                    center = "[" + senderShortImpl(ev.sender) + "] voted";
                } else if (mt == "m.sticker") {
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u2b1c sticker: " : "[sticker] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.audio") {
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u266a audio: " : "[audio] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.file") {
                    center = senderTag(st, st.currentRoomId, ev.sender)
                           + (st.showEmoji ? "\xf0\x9f\x93\x84 " : "[file] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.video") {
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u25b6 video: " : "[video] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.image") {
                    std::string dims;
                    auto info = ev.content.find("info");
                    if (info != ev.content.end() && info->is_object()) {
                        auto w = info->find("w");
                        auto h = info->find("h");
                        if (w != info->end() && h != info->end() &&
                            w->is_number() && h->is_number()) {
                            dims = " (" + std::to_string(w->get<int>()) + "x"
                                 + std::to_string(h->get<int>()) + ")";
                        }
                    }
                    // Images are compact by default; 'images on' shows the
                    // full card (dims).
                    std::string imgPrefix = st.showImages
                        ? (st.showEmoji ? "\u2b1c image: " : "[img] ")
                        : (st.showEmoji ? "\xf0\x9f\x96\xbc " : "[img] ");
                    center = "[" + senderShortImpl(ev.sender) + "] " + imgPrefix
                           + "\x1b[36m" + body + "\x1b[0m" + dims;
                }
            }
            std::string thr = eventThreadRoot(ev);
            std::string rep = eventReplyTo(ev);
            if (center.empty() && !thr.empty()) {
                std::string preview = eventPreview(st.db, st.currentRoomId, thr);
                center = "[" + chatName(st, st.currentRoomId, ev.sender) + "] \u2937 " + body
                       + "  (thread: " + (preview.empty() ? thr : preview) + ")";
            } else if (center.empty() && !rep.empty()) {
                // Element-style ReplyChain: walk the m.in_reply_to chain
                // (up to 3 levels) and stack the quoted previews, each
                // indented — the reply-of-reply shows both ancestors.
                std::string chain;
                std::string cur = rep;
                for (int lvl = 0; lvl < 3; ++lvl) {
                    matrix::Event prev;
                    if (!st.db->getEventById(cur, prev)) break;
                    std::string preview = eventBodyImpl(prev);
                    if (preview.empty()) break;
                    // The chain line uses the full effective panel width
                    // (the whole screen in mobile) — the wrap takes care of
                    // any overflow.
                    int chainW = st.mobile ? W : centerW;
                    chain += std::string(lvl, ' ') + "> ["
                           + chatName(st, st.currentRoomId, prev.sender)
                           + "] " + clip(preview, std::max(20, chainW)) + "\n";
                    auto rel = prev.content.find("m.relates_to");
                    if (rel == prev.content.end() || !rel->is_object()) break;
                    auto ir = rel->find("m.in_reply_to");
                    if (ir == rel->end() || !ir->is_object()) break;
                    auto eid = ir->find("event_id");
                    if (eid == ir->end() || !eid->is_string()) break;
                    cur = eid->get<std::string>();
                }
                center = "[" + chatName(st, st.currentRoomId, ev.sender) + "] " + body
                       + (chain.empty() ? "" : "\n" + chain);
            } else if (center.empty()) {
                center = senderTag(st, st.currentRoomId, ev.sender)
                       + (st.showLinks
                              ? highlightUrls(renderPermalinks(
                                    highlightMentions(
                                        renderMarkdownBody(body)),
                                    st.rooms, st.db))
                              : highlightMentions(renderMarkdownBody(body)));
                int rc = 0;
                for (const auto& ev2 : st.messages) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0) center += "  \u2937(" + std::to_string(rc) + ")";
            }
            // Redaction events render as system lines.
            if (ev.type == "m.room.redaction") {
                return "\xf0\x9f\x97\x91 " + senderShortImpl(ev.sender)
                     + " deleted a message";  // 🗑
            }
            // Redacted targets get the (deleted) marker.
            if (st.redactedIds.count(ev.event_id)) {
                center += "  (\xf0\x9f\x97\x91 deleted)";
            }
            // Poll response counts under the poll — with the option texts.
            if (!center.empty() &&
                center.find("poll:") != std::string::npos && ev.content.is_object()) {
                std::map<std::string, std::string> answerTexts;
                auto ans = ev.content.find("answers");
                if (ans != ev.content.end() && ans->is_array()) {
                    for (const auto& a : *ans) {
                        if (!a.is_object()) continue;
                        std::string id = a.value("id", "");
                        std::string text = a.value("text", "");
                        if (!id.empty()) answerTexts[id] = text;
                    }
                }
                std::map<std::string, int> votes;
                for (const auto& ev2 : st.messages) {
                    if (!ev2.content.is_object()) continue;
                    auto mt2 = ev2.content.find("msgtype");
                    if (mt2 == ev2.content.end() || !mt2->is_string() ||
                        mt2->get<std::string>() != "m.poll.response") continue;
                    auto rel2 = ev2.content.find("m.relates_to");
                    if (rel2 == ev2.content.end() || !rel2->is_object()) continue;
                    auto eid2 = rel2->find("event_id");
                    if (eid2 == rel2->end() || !eid2->is_string() ||
                        eid2->get<std::string>() != ev.event_id) continue;
                    auto sel = ev2.content.find("selections");
                    if (sel != ev2.content.end() && sel->is_array() &&
                        !sel->empty() && (*sel)[0].is_string()) {
                        votes[(*sel)[0].get<std::string>()]++;
                    }
                }
                if (!votes.empty()) {
                    std::string vstr;
                    for (const auto& [k, n] : votes) {
                        if (!vstr.empty()) vstr += ", ";
                        auto it = answerTexts.find(k);
                        std::string label = (it != answerTexts.end() && !it->second.empty())
                                                ? it->second : k;
                        vstr += label + ": " + std::to_string(n);
                    }
                    center += "  \xf0\x9f\x97\xb3 " + vstr;  // 🗳
                }
            }
            // Reactions and edits aggregated for EVERY message row.
            std::map<std::string, int> reacts;
            bool edited = false;
            for (const auto& ev2 : st.messages) {
                if (!ev2.content.is_object()) continue;
                auto rel = ev2.content.find("m.relates_to");
                if (rel == ev2.content.end() || !rel->is_object()) continue;
                auto rtype = rel->find("rel_type");
                auto teid = rel->find("event_id");
                if (rtype == rel->end() || teid == rel->end() ||
                    !rtype->is_string() || !teid->is_string()) continue;
                if (teid->get<std::string>() != ev.event_id) continue;
                std::string rt = rtype->get<std::string>();
                if (rt == "m.annotation") {
                    auto key = rel->find("key");
                    if (key != rel->end() && key->is_string()) {
                        reacts[key->get<std::string>()]++;
                    }
                } else if (rt == "m.replace") {
                    edited = true;
                }
            }
            for (const auto& [k, n] : reacts) {
                center += "  \x1b[32m" + k + " " + std::to_string(n) + "\x1b[0m";
            }
            if (edited) center += "  (edited)";
            return center;
        };
        int64_t prevDay = -1;
        // The last-read divider: the messages AFTER the marker count as
        // "new" (Element-style).
        int newAfterMarker = 0;
        if (!st.readMarker.empty()) {
            bool past = false;
            for (const auto& ev2 : st.messages) {
                if (!past) {
                    if (ev2.event_id == st.readMarker) past = true;
                    continue;
                }
                if (ev2.type == "m.room.message" || ev2.type == "m.sticker")
                    newAfterMarker++;
            }
        }
        for (const auto& ev : st.messages) {
            int64_t day = ev.origin_server_ts / 86400000;
            if (day != prevDay) {
                std::string label;
                std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000);
                std::tm tm{};
                localtime_r(&t, &tm);
                std::time_t nowT = std::time(nullptr);
                std::tm nowTm{};
                localtime_r(&nowT, &nowTm);
                char dateBuf[16];
                std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
                std::string dateStr(dateBuf);
                if (tm.tm_year == nowTm.tm_year && tm.tm_yday == nowTm.tm_yday) {
                    label = "Today (" + dateStr + ")";
                } else if (tm.tm_year == nowTm.tm_year && tm.tm_yday == nowTm.tm_yday - 1) {
                    label = "Yesterday (" + dateStr + ")";
                } else {
                    label = dateStr;
                }
                // The day's message count next to the date, like Element.
                int dayCount = 0;
                for (const auto& ev2 : st.messages) {
                    if (ev2.origin_server_ts / 86400000 != day) continue;
                    if (ev2.type != "m.room.message" && ev2.type != "m.sticker") continue;
                    dayCount++;
                }
                std::string sep = "── " + label + " ──  " + std::to_string(dayCount)
                                + " msgs";
                if (static_cast<int>(sep.size()) < centerW) {
                    sep = std::string((centerW - static_cast<int>(sep.size())) / 2, ' ') + sep;
                }
                centerRows.push_back(sep);
                prevDay = day;
            }
            // Element-style "viewing an earlier message" banner, right
            // above the jumped-to event.
            if (!st.focusEvent.empty() && ev.event_id == st.focusEvent) {
                std::string bar = "[44m â viewing event \xe2\x80\xb9"
                                + clip(st.focusEvent, 24) + "\xe2\x80\xba"
                                + " · 'newest' to return [0m";
                if (static_cast<int>(bar.size()) < centerW) {
                    bar = std::string(std::max(0, (centerW - static_cast<int>(bar.size())) / 2), ' ')
                        + bar;
                }
                centerRows.push_back(bar);
            }
            // The last-read divider: where the user last read up to, with
            // the count of the newer messages (the local m.fully_read copy).
            if (!st.readMarker.empty() && ev.event_id == st.readMarker) {
                std::string mark = "\x1b[33m── last read ──\x1b[0m";
                if (newAfterMarker > 0) {
                    mark += "  \x1b[90m" + std::to_string(newAfterMarker)
                          + " new\x1b[0m";
                }
                centerRows.push_back(mark);
            }
            // Element "show join/leave messages": member events are system
            // rows — hidden when the setting is off.
            if (!st.senderFilter.empty() && ev.type == "m.room.message" &&
                ev.sender != st.senderFilter &&
                ev.sender != "@" + st.senderFilter.substr(1)) continue;
            if (!st.showJoins && ev.type == "m.room.member") continue;
            // Reactions are aggregated into the message rows; the state
            // events (power levels, encryption, room meta) are system
            // data — neither should become standalone (empty) lines.
            if (ev.type == "m.reaction" || ev.type == "m.room.power_levels" ||
                ev.type == "m.room.encryption" || ev.type == "m.room.create" ||
                ev.type == "m.room.topic" || ev.type == "m.room.name" ||
                ev.type == "m.room.avatar" || ev.type == "m.room.canonical_alias" ||
                ev.type == "m.room.join_rules" ||
                ev.type == "m.room.history_visibility" ||
                ev.type == "m.room.encrypted" || ev.type == "m.room.redaction") {
                continue;
            }
            std::string row = renderRow(ev);
            if (!st.showNames) {
                // Element compact mode: hide the sender nicknames.
                if (row.size() >= 2 && row[0] == '[') {
                    auto close = row.find(']');
                    if (close != std::string::npos && close + 2 <= row.size() &&
                        row[close + 1] == ' ') {
                        row = row.substr(close + 2);
                    }
                }
            }
            if (!row.empty()) {
                auto rIt = st.receipts.find(ev.event_id);
                if (st.showReceipts && rIt != st.receipts.end() && !rIt->second.empty()) {
                    std::string rd = rIt->second;
                    // Cap the reader list: 'a b c +5' instead of the full set.
                    std::string shown;
                    int count = 0;
                    std::istringstream rss(rd);
                    std::string tok;
                    while (rss >> tok && count < 3) {
                        if (!shown.empty()) shown += " ";
                        shown += tok;
                        count++;
                    }
                    int total = 1;
                    for (char ch : rd) if (ch == ' ') total++;
                    if (count < total) shown += " +" + std::to_string(total - count);
                    row += "  \x1b[90m\u2713 " + shown + "\x1b[0m";  // ✓ readers
                }
                std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000)
                               + static_cast<std::time_t>(st.tzOffset) * 3600;
                std::tm tm{};
                localtime_r(&t, &tm);
                char buf[16];
                int h12 = tm.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                const char* ap = tm.tm_hour < 12 ? "AM" : "PM";
                if (st.clock12h) {
                    if (st.showSeconds) {
                        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d %s",
                                      h12, tm.tm_min, tm.tm_sec, ap);
                    } else {
                        std::snprintf(buf, sizeof(buf), "%d:%02d %s",
                                      h12, tm.tm_min, ap);
                    }
                } else if (st.showSeconds) {
                    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                                  tm.tm_hour, tm.tm_min, tm.tm_sec);
                } else {
                    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                                  tm.tm_hour, tm.tm_min);
                }
                std::string timeStr = buf;
                std::string head, body;
                if (st.msgNewline) {
                    // "HH:MM [nick] >" on its own line, the message below
                    // (WhatsApp-like) — only text rows have the "> " split.
                    auto gt = row.find("> ");
                    if (gt != std::string::npos) {
                        head = row.substr(0, gt + 1);
                        body = row.substr(gt + 2);
                    } else {
                        head = row;
                    }
                }
                std::string prefix;
                if (st.pinned.count(ev.event_id)) {
                    prefix += (st.showEmoji ? "\xf0\x9f\x93\x8c " : "[pin] ");
                }
                if (ev.event_id == st.focusEvent) {
                    prefix += "\x1b[7m \xe2\x97\x80 \x1b[0m";
                }
                std::string idSuffix;
                if (st.showIds) {
                    std::string shortId = ev.event_id.substr(0, 10);
                    if (!shortId.empty()) idSuffix = "  \u2039" + shortId + "\u203a";
                }
                // Long messages wrap to the panel width (word-aware, ANSI
                // safe); the continuation lines are indented under the
                // first one.
                // Wrap to the panel width minus the 8-cell continuation
                // indent, so indented lines never overflow the panel and
                // words are never cut at the edge.
                int wrapW = (st.mobile ? W : centerW) - 8;
                if (st.msgNewline && !body.empty()) {
                    std::string line0;
                    if (st.timeRight) {
                        std::string leftPart = prefix + head + idSuffix;
                        int padN = wrapW + 8 - displayWidth(leftPart)
                                 - displayWidth(timeStr);
                        line0 = leftPart + std::string(std::max(0, padN), ' ')
                              + timeStr;
                    } else {
                        line0 = prefix + timeStr + " " + head + idSuffix;
                    }
                    centerRows.push_back(line0);
                    auto bLines = wrapTextImpl(body, wrapW);
                    for (const auto& bl : bLines) {
                        centerRows.push_back(std::string(8, ' ') + bl);
                    }
                } else if (st.timeRight) {
                    // Reserve the time at the first line's right edge.
                    std::string inner = prefix + row + idSuffix;
                    int reserve = displayWidth(timeStr) + 1;
                    auto tLines = wrapTextImpl(inner, std::max(8, wrapW - reserve));
                    for (size_t li = 0; li < tLines.size(); ++li) {
                        if (li == 0) {
                            int padN = wrapW + 8 - displayWidth(tLines[0])
                                     - displayWidth(timeStr);
                            centerRows.push_back(tLines[0]
                                + std::string(std::max(0, padN), ' ') + timeStr);
                        } else {
                            centerRows.push_back(std::string(8, ' ') + tLines[li]);
                        }
                    }
                } else {
                    std::string first = prefix + timeStr + " " + row + idSuffix;
                    std::vector<std::string> lines = wrapTextImpl(first, wrapW);
                    for (size_t li = 0; li < lines.size(); ++li) {
                        if (li == 0) {
                            centerRows.push_back(lines[0]);
                        } else {
                            centerRows.push_back(std::string(8, ' ') + lines[li]);
                        }
                    }
                }
                // Thread master messages: their full id on its own line
                // (with --ids), so the roots are easy to reference.
                if (st.showIds && !ev.event_id.empty() &&
                    eventThreadRoot(ev) == ev.event_id) {
                    centerRows.push_back("      \xe2\xa4\xb7 thread root: "
                                         + clip(ev.event_id, centerW - 16));
                }
            }
        }
    }
    // Right panel content per mode: members | room thread list | one thread
    // | threads across all rooms (Element-style).
    std::vector<std::string> rightRows;
    if (st.rightPanel == 0) {
        // Full @user:server ids when the panel has the room for them.
        bool fullIds = false;
        if (!st.members.empty() && rightW >= 15) {
            int fullW = 0;
            for (const auto& mem : st.members) {
                int w = displayWidth(memberRowStr(st, mem, true));
                if (w > fullW) fullW = w;
            }
            fullIds = (rightW - 2 >= fullW);
        }
        for (const auto& mem : st.members) {
            rightRows.push_back(memberRowStr(st, mem, fullIds));
        }
        // The thread list sits at the BOTTOM of the right panel, under a
        // separator (Element-style): ⤷ <preview> (<reply count>).
        // The members get the top rows, the threads a window at the bottom
        // with its own scroll (--scroll-threads).
        if (st.showThreadsBottom && !st.currentRoomId.empty()) {
            std::vector<std::string> thr =
                roomThreadList(st.db, st.currentRoomId, rightW - 7, st.showIds);
            if (!thr.empty()) {
                // Reserve room for the threads: the member list gets capped
                // so the thread window (at least 1 row) always fits.
                int membersShown = static_cast<int>(rightRows.size());
                int wantThr = std::min(static_cast<int>(thr.size()),
                                       std::max(1, rows / 5));
                int membersCap = std::max(0, rows - wantThr - 1);
                if (membersShown > membersCap) rightRows.resize(membersCap);
                int thrWindow = std::max(1, rows - membersShown - 1);
                int ts = std::min(std::max(0, st.threadsScroll),
                                  std::max(0, static_cast<int>(thr.size()) - thrWindow));
                rightRows.push_back("----------");
                for (int k = 0; k < thrWindow && ts + k < static_cast<int>(thr.size());
                     ++k) {
                    rightRows.push_back(thr[static_cast<size_t>(ts + k)]);
                }
            }
        }
    } else if (st.rightPanel == 1) {
        // The room's threads: the roots with their reply counts (the
        // numbers select them: thread <room> <N>).
        auto evs = st.db->getEvents(st.threadRoomId, 300);
        int idx = 0;
        for (const auto& ev : evs) {
            int rc = 0;
            for (const auto& ev2 : evs) {
                if (eventThreadRoot(ev2) == ev.event_id) rc++;
            }
            if (rc > 0) {
                rightRows.push_back(std::to_string(idx + 1) + " \u2937 "
                                    + clip(eventBodyImpl(ev), 20) + " (" +
                                    std::to_string(rc) + ")");
                idx++;
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no threads in this room)");
    } else if (st.rightPanel == 2) {
        // One thread: the root + its replies (Element's thread panel),
        // wrapped to the panel width so long lines continue below.
        auto pushWrapped = [&](const std::string& line) {
            auto lines = wrapTextImpl(line, std::max(8, rightW - 1));
            for (size_t li = 0; li < lines.size(); ++li) {
                rightRows.push_back((li == 0 ? std::string()
                                             : std::string(4, ' ')) + lines[li]);
            }
        };
        if (!st.threadRootId.empty()) {
            for (const auto& ev : st.messages) {
                if (ev.event_id == st.threadRootId) {
                    pushWrapped("[" + senderShortImpl(ev.sender) + "] "
                                + eventBodyImpl(ev));
                }
            }
            for (const auto& ev : st.threadReplies) {
                pushWrapped("  \u2937 [" + senderShortImpl(ev.sender) + "] "
                            + eventBodyImpl(ev));
            }
        }
        if (rightRows.empty()) rightRows.push_back("(thread is empty)");
        // A small thread leaves free rows: fill them with the OTHER
        // threads and the members, so everything sits in one panel.
        if (static_cast<int>(rightRows.size()) < rows) {
            auto evs = st.db->getEvents(st.threadRoomId, 300);
            std::vector<std::string> thr;
            int idx = 0;
            for (const auto& ev : evs) {
                int rc = 0;
                for (const auto& ev2 : evs) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0) {
                    idx++;
                    if (ev.event_id == st.threadRootId) continue;
                    thr.push_back(std::to_string(idx) + " \u2937 "
                                  + clip(eventBodyImpl(ev), 20) + " (" +
                                  std::to_string(rc) + ")");
                }
            }
            int free = rows - static_cast<int>(rightRows.size());
            if (!thr.empty() && free > 3) {
                int thrN = std::min(static_cast<int>(thr.size()),
                                    std::max(1, free / 3));
                rightRows.push_back("----------");
                for (int k = 0; k < thrN; ++k) rightRows.push_back(thr[k]);
                free = rows - static_cast<int>(rightRows.size());
            }
            if (free > 2 && !st.members.empty()) {
                int mn = std::min(static_cast<int>(st.members.size()), free - 1);
                rightRows.push_back("----------");
                for (int k = 0; k < mn; ++k) {
                    rightRows.push_back(memberRowStr(st, st.members[k], true));
                }
            }
        }
    } else if (st.rightPanel == 3) {
        // Threads across ALL rooms.
        for (const auto& r : st.rooms) {
            std::string rid = r.value("room_id", "");
            std::string rname = r.value("name", rid);
            auto evs = st.db->getEvents(rid, 300);
            int shown = 0;
            for (const auto& ev : evs) {
                int rc = 0;
                for (const auto& ev2 : evs) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0 && shown < 3) {
                    rightRows.push_back(clip(rname, 10) + ": \u2937 "
                                        + clip(eventBodyImpl(ev), 16) + " (" +
                                        std::to_string(rc) + ")");
                    shown++;
                }
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no threads anywhere)");
    } else if (st.rightPanel == 4) {
        // The agent activity: the standing goal + the last conversation
        // (the auto-saved session). The static --right agent re-reads it.
        std::vector<agenttools::Message> hist;
        agenttools::loadSession(".agent-sessions/last.json", hist);
        agenttools::GoalState goal;
        agenttools::loadGoal(".agent-goal.json", goal);
        {
            // The current model + provider always visible here.
            agenttools::Config acfg;
            agenttools::loadAgentConfig(acfg);
            rightRows.push_back("[1m" + acfg.provider + " / "
                                + acfg.model + "[0m");
        }
        {
            std::ifstream tf(".agent-sessions/last.title");
            if (tf) {
                std::ostringstream tss;
                tss << tf.rdbuf();
                std::string title = tss.str();
                if (!title.empty()) rightRows.push_back(title);
            }
        }
        for (const auto& [stt, content] : agenttools::agentTodos()) {
            rightRows.push_back((stt == "in_progress" ? "→ " :
                                 stt == "completed" ? "✓ " : "· ") + content);
        }
        if (!goal.goal.empty()) {
            rightRows.push_back(clip("goal: " + goal.goal, rightW - 1));
            for (const auto& sg : goal.subgoals) {
                rightRows.push_back(clip("  ▸ " + sg, rightW - 1));
            }
            rightRows.push_back("----------");
        }
        auto pushWrapped = [&](const std::string& line) {
            auto lines = wrapTextImpl(line, std::max(8, rightW - 1));
            for (size_t li = 0; li < lines.size(); ++li) {
                rightRows.push_back((li == 0 ? std::string()
                                             : std::string(2, ' ')) + lines[li]);
            }
        };
        int n = static_cast<int>(hist.size());
        int shown = 0;
        for (int i = std::max(0, n - 24); i < n && shown < 24; ++i, ++shown) {
            const auto& m = hist[static_cast<size_t>(i)];
            if (m.role == "user") {
                pushWrapped("▸ " + m.content);
            } else if (m.role == "assistant") {
                pushWrapped("◂ " + m.content);
            } else if (m.role == "tool") {
                pushWrapped("  · " + m.toolName + " → " + m.content);
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no agent activity yet)");
    }
    // Smartphone mode: the rooms list, the chat and the members become one
    // long stacked stream (section separators between them) — the phone
    // scrolls it like a web page.
    if (st.mobile) {
        // Element Classic style: ONE pane at a time — Rooms / Chat / People —
        // chosen by the bottom navigation bar. The pane scrolls on its own.
        std::vector<std::string> stream;
        std::string section;
        if (st.mobileTab == 0) {
            // Invited rooms get their own section at the top, like Element.
            std::vector<std::string> invRows;
            std::vector<std::string> allRows;
            for (const auto* r : visible) {
                std::string rid = r->value("room_id", "");
                std::string mark = rid == st.currentRoomId ? "*" : " ";
                std::string name = roomDisplayNameImpl(*r);
                if (r->value("is_direct", false))
                    name = (st.showEmoji ? "💬 " : "[DM] ") + name;
                if (st.invited.count(rid)) {
                    mark += (st.showEmoji ? "📨 " : "[INV] ");
                }
                std::vector<std::string>& dst = st.invited.count(rid) ? invRows : allRows;
                std::string row = mark + "[1m" + name + "[0m ("
                                + std::to_string(roomMessageCount(st.db, rid)) + ")";
                if (r->value("is_encrypted", false)) {
                    row += (st.showEmoji ? " 🔒" : " [E2EE]");
                }
                if (st.mutedRooms.count(rid)) {
                    row += (st.showEmoji ? " 🔇" : " [muted]");
                }
                if (st.starredRooms.count(rid)) {
                    row += " ★";  // ★
                }
                // The room description (topic) in dim, after the count.
                {
                    std::string topic = r->value("topic", "");
                    if (!topic.empty()) {
                        int used = displayWidth(row) + 2;
                        row += "[90m" + clip(" · " + topic,
                                                 std::max(4, W - 8 - used))
                             + "[0m";
                    }
                }
                std::string ltime = roomLastTime(st.db, rid, st.showSeconds,
                                                   st.clock12h);
                // For invited rooms the time slot shows the remembered
                // invitation date instead of the last message time.
                {
                    auto invIt = st.inviteByRoom.find(rid);
                    if (invIt != st.inviteByRoom.end() && invIt->second.ts > 0) {
                        ltime = "invited " + relativeTime(invIt->second.ts);
                        if (st.db->getSetting("invreason_menu", "0") != "0" &&
                            !invIt->second.reason.empty()) {
                            ltime += " — " + invIt->second.reason;
                        }
                    }
                }
                if (!ltime.empty()) {
                    int tl = displayWidth(ltime);
                    int baseW = W - tl - 1;
                    row = clip(row, baseW);
                    row += std::string(std::max(1, W - baseW - tl), ' ')
                         + "[90m" + ltime + "[0m";
                }
                int thr = st.db->getSetting("threads_off", "0") == "0"
                              ? roomThreadCount(st.db, rid) : 0;
                if (thr > 0) {
                    row += (st.showEmoji ? " 🧵" : " (threads ")
                         + std::to_string(thr) + (st.showEmoji ? "" : ")");
                }
                dst.push_back(row);
                std::string last = roomLastMsg(st.db, rid, st.rooms);
                if (!last.empty()) {
                    auto colon = last.find(':');
                    std::string who = colon == std::string::npos
                                          ? last : last.substr(0, colon + 1);
                    std::string what = colon == std::string::npos
                                           ? "" : last.substr(colon + 1);
                    dst.push_back("  " + who + "[90m"
                                + highlightMentions(
                                      clip(what,
                                           std::max(2, W - 3 - displayWidth(who))))
                                + "[0m");
                }
            }
            if (!invRows.empty()) {
                stream.push_back("📨 Invites");
                stream.insert(stream.end(), invRows.begin(), invRows.end());
                stream.push_back("-- Rooms --");
            }
            stream.insert(stream.end(), allRows.begin(), allRows.end());
            section = " Rooms ";
        } else if (st.mobileTab == 1) {
            stream.push_back(clip("── " + roomName + " ──", std::max(1, W - 1)));
            stream.insert(stream.end(), centerRows.begin(), centerRows.end());
            section = " Chat ";
        } else {
            stream.insert(stream.end(), rightRows.begin(), rightRows.end());
            section = " People ";
        }
        int total = static_cast<int>(stream.size());
        int maxScroll = std::max(0, total - rows);
        int scroll = std::min(std::max(0, st.scroll), maxScroll);
        if (scroll > 0) out += "  ^ more above (scroll up)\n";
        if (scroll + rows < total) out += "  v more below (scroll down)\n";
        for (int i = 0; i < rows; ++i) {
            int src = scroll + i;
            out += (src < total) ? clip(stream[static_cast<size_t>(src)], std::max(1, W - 1)) + "\n"
                                 : "\n";
        }
        out += repeat('=', std::max(0, W - 1)) + "\n";
        // Bottom navigation (Element Classic style): the active tab is
        // bracketed, the others are bare.
        std::string nav;
        for (int t = 0; t < 3; ++t) {
            const char* label = (t == 0) ? "Rooms" : (t == 1) ? "Chat" : "People";
            int padn = (W - 18) / 2 - 2;  // ~centered
            if (t == st.mobileTab) {
                nav += "\x1b[7m" + std::string(label) + "\x1b[0m";
            } else {
                nav += std::string(label);
            }
            if (t < 2) nav += "  ";
        }
        std::string padnav = std::string(std::max(0, (W - 20) / 2), ' ') + nav;
        out += clip(padnav, W) + "\n";
        std::string pos;
        if (total > rows) {
            pos = " [rows " + std::to_string(scroll + 1) + "-"
                + std::to_string(scroll + rows) + " of "
                + std::to_string(total) + "]";
        }
        out += "proxy: " + st.proxyLabel + " | scroll: up/down/top/bottom" + pos;
        if (!st.statusNote.empty()) out += " | " + st.statusNote;
        out += "\n";
        out += "rooms | chat | people | open <room> | send <room> <text> |"
               " find <q> | search <q> | settings | help | quit\n";
        return out;
    }

    // Clamp the scroll into [0, contentRows - rows] (state stays const).
    int visCount = st.roomFilter.empty() ? contentRowsImpl(st)
                                         : static_cast<int>(visible.size());
    visCount = std::max(visCount, static_cast<int>(centerRows.size()));
    visCount = std::max(visCount, static_cast<int>(rightRows.size()));
    int maxScroll = std::max(0, visCount - rows);
    int scroll = std::min(std::max(0, st.scroll), maxScroll);
    // The rooms list can scroll on its own (--scroll-left): only the
    // left panel moves, the chat and members stay put.
    int leftScroll = std::min(std::max(0, st.leftScroll),
                              std::max(0, static_cast<int>(visible.size()) - rows));
    if (scroll > 0) out += "  ^ more above (scroll up)\n";
    if (scroll + rows < contentRowsImpl(st)) out += "  v more below (scroll down)\n";
    for (int i = 0; i < rows; ++i) {
        int src = scroll + i;  // the content row this view row shows
        int leftSrc = leftScroll + i;  // the rooms row (scrolled separately)
        std::string left, center, right;
        if (leftSrc < static_cast<int>(visible.size())) {
            const auto& r = *visible[static_cast<size_t>(leftSrc)];
            std::string rid = r.value("room_id", "");
            std::string mark = rid == st.currentRoomId ? "*" : " ";
            std::string name = roomDisplayNameImpl(r);
            if (r.value("is_direct", false))
                name = (st.showEmoji ? "💬 " : "[DM] ") + name;
            left = mark + "[1m" + name + "[0m ("
                 + std::to_string(roomMessageCount(st.db, rid)) + ")";
            // Invited rooms: the remembered invitation date replaces the
            // last-message preview below ("📨 2h ago").
            auto invIt = st.inviteByRoom.find(rid);
            if (r.value("is_encrypted", false)) {
                left += (st.showEmoji ? " 🔒" : " [E2EE]");
            }
            if (st.mutedRooms.count(rid)) {
                left += (st.showEmoji ? " 🔇" : " [muted]");
            }
            if (st.starredRooms.count(rid)) {
                left += " ★";  // ★
            }
            int thr = st.db->getSetting("threads_off", "0") == "0"
                          ? roomThreadCount(st.db, rid) : 0;
            if (thr > 0) {
                left += (st.showEmoji ? " 🧵" : " (threads ") + std::to_string(thr)
                      + (st.showEmoji ? "" : ")");
            }
            if (invIt != st.inviteByRoom.end()) {
                // No preview: the row shows when the invitation arrived
                // ("📨 2h ago") — the compact form fits the narrow panel.
                left += " [90m📨 " + relativeTime(invIt->second.ts) + "[0m";
            } else {
                // The last message preview, like Element's room list.
                std::string last = roomLastMsg(st.db, rid, st.rooms);
                if (!last.empty()) {
                    // The preview leaves room for the last-message time at
                    // the row's end (the Element-style room list).
                    int avail = leftW - displayWidth(left) - 9;
                    if (avail >= 6) {
                        // Nickname in the normal color, the message dimmed.
                        auto colon = last.find(':');
                        std::string who = colon == std::string::npos
                                              ? last : last.substr(0, colon + 1);
                        std::string what = colon == std::string::npos
                                               ? "" : last.substr(colon + 1);
                        int used = displayWidth(who) + 2;  // the " · " separator
                        left += " [90m· [0m" + who
                              + "[90m"
                              + highlightMentions(clip(what, std::max(2, avail - used)))
                              + "[0m";
                        left += " [90m"
                              + roomLastTime(st.db, rid, st.showSeconds,
                                             st.clock12h)
                              + "[0m";
                    }
                }
            }
        }
        if (src < static_cast<int>(centerRows.size())) {
            center = centerRows[static_cast<size_t>(src)];
        }
        if (src < static_cast<int>(rightRows.size())) {
            right = rightRows[static_cast<size_t>(src)];
        }
        std::string rightOut = clip(right, static_cast<size_t>(rightW));
        if (rightOut.find('\x1b') != std::string::npos) rightOut += "\x1b[0m";
        // The right panel gets no trailing padding: a row of exactly W
        // cells makes Konsole wrap onto a phantom blank line.
        out += pad(left, static_cast<size_t>(leftW)) + PIPE + "|" + X
             + pad(center, static_cast<size_t>(centerW)) + PIPE + "|" + X
             + rightOut + "\n";
    }

    // Status line
    out += repeat('=', std::max(0, W - 1)) + "\n";
    std::string pos;
    if (contentRowsImpl(st) > rows) {
        pos = " [rows " + std::to_string(scroll + 1) + "-"
            + std::to_string(scroll + rows) + " of "
            + std::to_string(contentRowsImpl(st)) + "]";
    }
    out += "proxy: " + st.proxyLabel + " | scroll: up/down/top/bottom" + pos;
    if (!st.statusNote.empty()) {
        // The last action's summary lives in the RIGHT-BOTTOM corner.
        out += " | " + st.statusNote;
    }
    out += "\n";
    out += "open <room> | send <room> <text> | find <q> | search <q> | thread |"
           " dump <room> | settings | space | help | quit\n";
    return out;
}

} // namespace


// ---- the cross-TU wrappers (the settings commands in ascii_settings.cpp) ----

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

// The about screen: the title, the (c) line, the user's aewan logo
// (the rising line with the /\ dip), the tagline and the info.
void printAbout(const std::string& proxyLabel, const std::string& accountLabel) {
    const char* X = "\x1b[0m";
    std::cout << "\x1b[1;31mprogressive-cli\x1b[0m"
              << " — the Matrix chat client and coding agent, designed to work in the terminal\n";
    std::cout << "(c) Progressive Chat contributors\n\n";
    const char* BR = "\x1b[1;31m";  // bold red
    std::cout << BR << "       /\n";
    std::cout << "      /\n";
    std::cout << "     /\n";
    std::cout << "    /\n";
    std::cout << " /\\/\n";
    std::cout << "/" << X << "\n";
    std::cout << "      — chat progress, always increasing —\n\n";
    std::cout << "Version: 0.5.0\n";
    std::cout << "License: AGPL-3.0\n";
    std::cout << "https://github.com/progressive-chat/progressive-cli\n";
    (void)proxyLabel; (void)accountLabel;
}



// ---- Mini line editor with the command history ----
// The terminal is switched to raw mode so the arrow keys arrive as escape
// sequences; the line is rendered by us. Restores the terminal on exit.
bool readLineWithHistory(std::vector<std::string>& history,
                         const std::string& prompt, std::string& out) {
    out.clear();
    bool isTty = isatty(STDIN_FILENO);
    struct termios oldt{};
    if (isTty && tcgetattr(STDIN_FILENO, &oldt) != 0) isTty = false;
    struct termios raw = oldt;
    if (isTty) {
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    auto restore = [&]() {
        if (isTty) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    };
    if (!isTty) {
        // Piped input: no arrows, plain getline.
        std::cout << prompt << std::flush;
        if (!std::getline(std::cin, out)) return false;
        return true;
    }
    std::cout << prompt << std::flush;
    size_t hpos = history.size();
    auto render = [&]() {
        std::cout << "\r\033[K" << prompt << out << std::flush;
    };
    bool ok = true;
    for (;;) {
        int c = getchar();
        if (c == EOF) {  // the Ctrl+C EINTR (or the stream end)
            ok = false;
            break;
        }
        if (c == '\x03') {  // Ctrl+C
            ok = false;
            break;
        }
        if (c == '\n') {    // Enter
            std::cout << "\n";
            break;
        }
        if (c == 0x7f || c == 0x08) {  // Backspace
            if (!out.empty()) { out.pop_back(); render(); }
            continue;
        }
        if (c == '\x1b') {   // Escape sequences (arrow keys)
            int a = getchar();
            if (a != '[') continue;
            int b = getchar();
            if (b == 'A') {   // Up
                if (hpos > 0) {
                    hpos--;
                    out = history[hpos];
                    render();
                }
            } else if (b == 'B') {  // Down
                if (hpos < history.size()) {
                    hpos++;
                    out = (hpos < history.size()) ? history[hpos] : "";
                    render();
                }
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            out += static_cast<char>(c);
            render();
        }
    }
    restore();
    if (ok && !out.empty() && (history.empty() || history.back() != out)) {
        history.push_back(out);
    }
    return ok;
}

// One-shot 'progressive-cli about' — the about screen without the ui.
int cmdAbout(const cli::Args&) {
    // No session/network init — the about screen must be clean and instant.
    printAbout("", "");
    return 0;
}

int cmdAsciiUi(const cli::Args& args) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cerr << "Cannot open matrixcli.db — run 'progressive-cli demo populate' first"
                  << std::endl;
        return 1;
    }

    UiState st;
    st.db = &dbi;
    st.rooms = dbi.listRooms();
    sortRoomsByActivity(st);
    if (st.rooms.empty()) {
        std::cerr << "No rooms cached. Run 'progressive-cli demo populate' or sync first."
                  << std::endl;
        return 1;
    }
    // Who are we? The saved session's user id, or "demo (offline)".
    if (pcore::init() && pcore::loadSavedSession()) {
        std::string uid = pcore::core().client->account().userId;
        if (!uid.empty()) {
            if (uid[0] == '@') uid = uid.substr(1);
            st.accountLabel = uid;  // e.g. "bob@matrix.org"
        }
    }
    if (st.accountLabel.empty()) st.accountLabel = "demo (offline)";
    st.proxyLabel = proxyLabelText();
    // Invites count for the header: the demo user is "@you", sessions use
    // the saved mxid ("@" stripped in accountLabel).
    st.invites = dbi.inviteCount(st.accountLabel == "demo (offline)"
                                     ? "@you" : "@" + st.accountLabel);
    for (const auto& id : dbi.invitedRoomIds(st.accountLabel == "demo (offline)"
                                                 ? "@you" : "@" + st.accountLabel)) {
        st.invited.insert(id);
    }
    // The remembered invitation dates (the cache keeps the invite events
    // with their timestamps).
    for (const auto& inv : dbi.openInvites(st.accountLabel == "demo (offline)"
                                               ? "@you" : "@" + st.accountLabel)) {
        st.inviteByRoom[inv.roomId] = inv;
    }
    // Persisted settings (the Settings screen): restore them on start.
    st.showSeconds = dbi.getSetting("time_full") == "1";
    st.showIds = dbi.getSetting("ids") == "1";
    st.showImages = dbi.getSetting("images") == "1";
    st.showEmoji = dbi.getSetting("emoji") != "0";
    st.limitRows = std::max(0, std::atoi(dbi.getSetting("rows", "0").c_str()));
    try { st.leftPanelW = std::stoi(dbi.getSetting("panel_left", "-1")); } catch (...) {}
    try { st.rightPanelW = std::stoi(dbi.getSetting("panel_right", "-1")); } catch (...) {}
    st.mobile = dbi.getSetting("mobile") == "1";
    // Auto: a narrow terminal (< 60 columns) cannot fit the three
    // columns, so the smartphone layout kicks in by itself.
    if (terminalWidthImpl() < 60) {
        st.mobile = true;
        st.statusNote = "narrow terminal · smartphone layout auto-enabled";
    }
    st.showNames = dbi.getSetting("names") != "0";
    st.showReceipts = dbi.getSetting("receipts") != "0";
    st.showJoins = dbi.getSetting("joins") != "0";
    st.showLinks = dbi.getSetting("links") != "0";
    st.clock12h = dbi.getSetting("clock12h") == "1";
    st.timeRight = dbi.getSetting("time_side") == "right";
    st.msgNewline = dbi.getSetting("msg_line") == "newline";
    st.autoPanels = dbi.getSetting("panel_auto") != "0";
    try { st.membersMode = std::stoi(dbi.getSetting("members_mode", "0")); } catch (...) {}
    st.showThreadsBottom = dbi.getSetting("threads_bottom") != "0";
    try { st.viaLimit = std::stoi(dbi.getSetting("via_limit", "3")); } catch (...) {}
    try { st.tzOffset = std::stoi(dbi.getSetting("tz_offset", "0")); } catch (...) {}
    try { st.hiddenSeconds = std::stoi(dbi.getSetting("hidden_seconds", "12")); } catch (...) {}
    auto loadMap = [&](const std::string& key, std::map<std::string, std::string>& out) {
        std::string v = dbi.getSetting(key, "");
        std::string cur;
        for (char ch : v) {
            if (ch == ',') {
                auto eq = cur.find('=');
                if (eq != std::string::npos) out[cur.substr(0, eq)] = cur.substr(eq + 1);
                cur.clear();
            } else cur += ch;
        }
        auto eq = cur.find('=');
        if (eq != std::string::npos) out[cur.substr(0, eq)] = cur.substr(eq + 1);
    };
    loadMap("room_nicks", st.roomNicks);
    loadMap("room_avatars", st.roomAvatars);
    loadMap("user_colors", st.userColors);
    {
        std::string v = dbi.getSetting("muted", "");
        std::string cur;
        for (char ch : v) { if (ch == ',') { st.mutedRooms.insert(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty()) st.mutedRooms.insert(cur);
    }
    {
        std::string v = dbi.getSetting("starred", "");
        std::string cur;
        for (char ch : v) { if (ch == ',') { st.starredRooms.insert(cur); cur.clear(); } else cur += ch; }
        if (!cur.empty()) st.starredRooms.insert(cur);
    }
    // The starred rooms must be known before the list is sorted.
    sortRoomsByActivity(st);
    // Custom character widths ("widths" setting): "cp:width,cp:width".
    {
        g_widthOverrides.clear();
        auto parseOne = [](const std::string& seg) {
            auto colon = seg.find(':');
            if (colon == std::string::npos) return;
            uint32_t cp = static_cast<uint32_t>(
                std::strtoul(seg.substr(0, colon).c_str(), nullptr, 16));
            int wd = std::atoi(seg.substr(colon + 1).c_str());
            if (cp > 0 && (wd == 1 || wd == 2)) {
                g_widthOverrides[cp] = wd;
            }
        };
        std::string w = dbi.getSetting("widths", "");
        std::string cur;
        for (char ch : w) {
            if (ch == ',') {
                parseOne(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        parseOne(cur);  // the last (or only) entry
    }
    // Demo/offline: static presence so the right panel shows the letters.
    // The demo events carry short senders ("@alice") — key both forms.
    if (st.accountLabel == "demo (offline)") {
        struct { const char* full; const char* short_; const char* letter; } demoPres[] = {
            {"@alice:demo.local", "@alice", "O"},
            {"@bob:demo.local", "@bob", "A"},
            {"@charlie:demo.local", "@charlie", "F"},
            {"@you:demo.local", "@you", "O"},
            {"@carol:demo.local", "@carol", "O"},
            {"@dave:demo.local", "@dave", "O"},
            {"@erin:demo.local", "@erin", "A"},
            {"@frank:demo.local", "@frank", "F"},
            {"@grace:demo.local", "@grace", "O"},
            {"@heidi:demo.local", "@heidi", "O"},
            {"@ivan:demo.local", "@ivan", "A"},
            {"@julia:demo.local", "@julia", "O"},
            {"@kate:demo.local", "@kate", "F"},
        };
        for (auto& p : demoPres) {
            st.presence[p.full] = p.letter;
            st.presence[p.short_] = p.letter;
        }
    }
    std::string initial = args.positional.empty() ? "" : args.positional[0];
    loadRoomIntoStateImpl(st, initial);
    if (st.currentRoomId.empty() && !st.rooms.empty()) {
        loadRoomIntoStateImpl(st, st.rooms.front().value("room_id", ""));
    }


    // --static thread <room> / --static threads: the right panel becomes
    // the thread list instead of a room being opened.
    if (initial == "thread" && args.positional.size() >= 2) {
        loadRoomIntoStateImpl(st, args.positional[1]);
        st.rightPanel = 1;
        st.threadRoomId = st.currentRoomId;
    } else if (initial == "threads") {
        st.rightPanel = 3;
        if (args.positional.size() >= 2) loadRoomIntoStateImpl(st, args.positional[1]);
    }
    // Non-interactive flags (also usable in the REPL): --ids, --time-full,
    // --right members|threads, --limit N.
    if (args.options.count("ids")) st.showIds = true;
    if (args.options.count("time-full") || args.options.count("sec")) st.showSeconds = true;
    if (args.options.count("time-side")) {
        st.timeRight = args.options.at("time-side") == "right";
    }
    if (args.options.count("msg-line")) {
        st.msgNewline = args.options.at("msg-line") == "newline";
    }
    if (args.options.count("no-emoji")) st.showEmoji = false;
    if (args.options.count("limit")) {
        try { st.limit = std::stoi(args.options.at("limit")); } catch (...) {}
        loadRoomIntoStateImpl(st, std::string(st.currentRoomId));
    }
    if (args.options.count("right")) {
        std::string r = args.options.at("right");
        if (r == "threads") {
            st.rightPanel = 3;
        } else if (r == "agent") {
            st.rightPanel = 4;
        } else if (r == "list") {
            st.rightPanel = 1;
            st.threadRoomId = st.currentRoomId;
        } else if (r == "thread") {
            std::string sel;
            if (args.options.count("thread-root")) sel = args.options.at("thread-root");
            else if (args.options.count("thread")) sel = args.options.at("thread");
            st.rightPanel = 2;
            st.threadRoomId = st.currentRoomId;
            st.threadRootId = sel;
            std::string root = resolveThreadRoot(st.db, st.currentRoomId, sel);
            if (root.empty()) {
                st.rightPanel = 0;
                st.statusNote = "thread not found: " + sel;
            } else {
                st.threadRootId = root;
                auto events = st.db->getEvents(st.currentRoomId, 300);
                st.threadReplies.clear();
                for (const auto& ev : events) {
                    if (eventThreadRoot(ev) == root) st.threadReplies.push_back(ev);
                }
            }
        }
    }
    // --thread <N|id>: the thread panel of the open room, no --right needed.
    if (args.options.count("thread") && st.rightPanel != 2) {
        std::string sel = args.options.at("thread");
        std::string root = resolveThreadRoot(st.db, st.currentRoomId, sel);
        if (root.empty()) {
            st.statusNote = "thread not found: " + sel;
        } else {
            st.rightPanel = 2;
            st.threadRoomId = st.currentRoomId;
            st.threadRootId = root;
            auto events = st.db->getEvents(st.currentRoomId, 300);
            st.threadReplies.clear();
            for (const auto& ev : events) {
                if (eventThreadRoot(ev) == root) st.threadReplies.push_back(ev);
            }
        }
    }

    if (args.options.count("static") || args.options.count("once") ||
        args.options.count("print")) {
        st.staticFrame = true;
    }
    // Settings for the one-shot frame: --rows N (frame height) and
    // --scroll N (viewport offset) — the room list scrolls within it.
    if (args.options.count("mobile")) st.mobile = true;
    if (args.options.count("space")) {
        st.activeSpace = resolveSpace(st.rooms, args.options.at("space"));
    }
    // Temporary one-shot layout flags (not persisted, unlike the REPL
    // commands panel/members/rows which save to the settings table):
    // --panel-left/--panel-right <off|on|width>, --panel-auto on|off,
    // --members horizontal|list|auto.
    auto parsePanel = [](const std::string& v) -> int {
        if (v == "off") return 0;
        if (v == "on" || v.empty()) return -1;
        try { return std::stoi(v); } catch (...) { return -1; }
    };
    if (args.options.count("panel-left")) {
        st.leftPanelW = parsePanel(args.options.at("panel-left"));
        st.autoPanels = false;
    }
    if (args.options.count("panel-right")) {
        st.rightPanelW = parsePanel(args.options.at("panel-right"));
        st.autoPanels = false;
    }
    if (args.options.count("panel-auto")) {
        st.autoPanels = args.options.at("panel-auto") != "off";
    }
    if (args.options.count("members")) {
        std::string m = args.options.at("members");
        if (m == "horizontal") st.membersMode = 1;
        else if (m == "list" || m == "vertical") st.membersMode = 2;
        else st.membersMode = 0;
    }
    // Element Classic: with a room on the command line, open it in the
    // Chat tab right away; without one, land on the Rooms tab.
    if (st.mobile && !initial.empty()) st.mobileTab = 1;
    if (args.options.count("rows")) {
        try { st.limitRows = std::stoi(args.options.at("rows")); } catch (...) {}
    }
    if (args.options.count("scroll")) {
        try { st.scroll = std::stoi(args.options.at("scroll")); } catch (...) {}
    }
    // --jump <YYYY-MM-DD>: position the viewport at that day (static).
    if (args.options.count("jump")) {
        int64_t dayMs = parseDayMsImpl(args.options.at("jump"));
        if (dayMs > 0) {
            int64_t best = 0;
            std::string bestId;
            for (const auto& ev : st.messages) {
                if (ev.origin_server_ts >= dayMs) {
                    best = ev.origin_server_ts;
                    bestId = ev.event_id;
                    break;
                }
            }
            if (!bestId.empty()) {
                int row = centerRowIndexOfImpl(st, bestId);
                if (row >= 0) {
                    st.scroll = std::max(0, row - 12);
                    st.focusEvent = bestId;
                    st.statusNote = "jumped to " + args.options.at("jump");
                }
            }
        }
    }
    if (args.options.count("scroll-left")) {
        try { st.leftScroll = std::stoi(args.options.at("scroll-left")); } catch (...) {}
    }
    if (args.options.count("scroll-threads")) {
        try { st.threadsScroll = std::stoi(args.options.at("scroll-threads")); } catch (...) {}
    }
    std::cout << drawFrameImpl(st) << std::flush;

    // --agent <prompt>: run the local coding agent after the frame.
    if (args.options.count("agent")) {
        agenttools::Config cfg;
        cfg.provider = dbi.getSetting("agent_provider", "openai");
        cfg.endpoint = dbi.getSetting("agent_endpoint", "");
        cfg.model = dbi.getSetting("agent_model", "");
        cfg.key = dbi.getSetting("agent_key", "");
        cfg.trust = dbi.getSetting("agent_trust", "ask");
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
        }
        if (args.options.count("agent-provider")) cfg.provider = args.options.at("agent-provider");
        if (args.options.count("agent-endpoint")) cfg.endpoint = args.options.at("agent-endpoint");
        if (args.options.count("agent-model")) cfg.model = args.options.at("agent-model");
        if (args.options.count("agent-key")) cfg.key = args.options.at("agent-key");
        if (args.options.count("agent-trust")) cfg.trust = args.options.at("agent-trust");
        if (args.options.count("agent-proxy")) cfg.proxy = args.options.at("agent-proxy");
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
        char cwdbuf[4096];
        if (getcwd(cwdbuf, sizeof(cwdbuf))) cfg.cwd = cwdbuf;
        std::vector<agenttools::Message> history;
        agenttools::Result res = agenttools::run(cfg, args.options.at("agent"),
            history,
            [&](const std::string& cmd) -> int {
                std::cout << "run: " << cmd << " [y/N/a/A] " << std::flush;
                std::string ans;
                std::getline(std::cin, ans);
                if (ans == "y" || ans == "Y") return 1;
                if (ans == "a") return 2;
                if (ans == "A") return 3;
                return 0;
            },
            [&](const std::string& questionsJson) -> std::string {
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
            },
            [](const std::string& l) { std::cout << l << std::endl; },
            [](const std::string& t) { std::cout << t << std::flush; });
        if (!res.ok) std::cout << "[agent error] " << res.error << std::endl;
        else if (!res.streamed) std::cout << res.text << std::endl;
        mkdir(".agent-sessions", 0755);
        agenttools::saveSession(".agent-sessions/last.json", history);
    }

    // Pure CLI / non-interactive mode: draw the frame once and exit
    // (pipe-friendly: progressive-cli ui --static [room] | less).
    if (args.options.count("static") || args.options.count("once") ||
        args.options.count("print")) {
        // Static mode: media previews only with --media (opt-in).
        if (args.options.count("media")) {
        for (const auto& ev : st.messages) {
            if (ev.type != "m.room.message" && ev.type != "m.sticker") continue;
            if (!ev.content.is_object()) continue;
            bool isImage = false;
            auto mt = ev.content.find("msgtype");
            if (mt != ev.content.end() && mt->is_string() &&
                mt->get<std::string>() == "m.image") isImage = true;
            if (ev.type == "m.sticker") isImage = true;
            if (!isImage) continue;
            std::string mxc;
            auto urlIt = ev.content.find("url");
            if (urlIt != ev.content.end() && urlIt->is_string()) mxc = urlIt->get<std::string>();
            if (mxc.empty()) continue;
            std::string fname = "media_" + ev.event_id.substr(0, 12) + ".bin";
            if (mxc.find("demo.local") != std::string::npos) {
                std::cout << "── " << eventBodyImpl(ev) << " (demo preview) ──" << std::endl;
                std::string tmpImg = "/tmp/matrixcli_preview.png";
                std::string gen = "magick -size 320x160 gradient:orange-red "
                    "-fill white -pointsize 20 -gravity center "
                    "-annotate 0 'demo image' '" + tmpImg
                    + "' 2>/dev/null || convert -size 320x160 "
                    "gradient:orange-red -fill white -pointsize 20 "
                    "-gravity center -annotate 0 'demo image' '" + tmpImg
                    + "' 2>/dev/null";
                std::system(gen.c_str());
                std::string cmd;
                if (std::system("which chafa >/dev/null 2>&1") == 0) {
                    cmd = "chafa --format symbols --size 60x20 '" + tmpImg + "' 2>/dev/null";
                } else if (std::system("which img2txt >/dev/null 2>&1") == 0) {
                    cmd = "img2txt -W 60 -H 20 '" + tmpImg + "' 2>/dev/null";
                }
                if (!cmd.empty()) std::system(cmd.c_str());
                std::remove(tmpImg.c_str());
            }
        }
        }
        return 0;
    }

    std::string line;
    std::vector<std::string> history;
    for (;;) {
        if (!matrixcli::g_interrupted.load()) break;  // Ctrl+C
        if (!readLineWithHistory(history, "ui> ", line)) break;
        if (!matrixcli::g_interrupted.load()) break;
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        cli::Args a;
        {
            std::istringstream iss(line);
            std::vector<std::string> words;
            std::string w;
            while (iss >> w) words.push_back(w);
            a.command = words[0];
            static const std::unordered_set<std::string> noValueFlags = {
                "static", "once", "print", "json", "confirm", "debug", "ts",
                "ids", "expand", "verbose", "all", "interactive", "help",
                "cli", "ui", "ascii", "populate", "no-replies", "no-filter",
            };
            for (size_t i = 1; i < words.size(); ++i) {
                if (words[i].size() >= 2 && words[i][0] == '-' && words[i][1] == '-') {
                    std::string key = words[i].substr(2);
                    auto eq = key.find('=');
                    if (eq != std::string::npos) {
                        a.options[key.substr(0, eq)] = key.substr(eq + 1);
                    } else if (i + 1 < words.size() && !noValueFlags.count(key) &&
                               !(words[i + 1].size() >= 2 && words[i + 1][0] == '-')) {
                        a.options[key] = words[++i];
                    } else {
                        a.options[key] = "true";
                    }
                } else {
                    a.positional.push_back(words[i]);
                }
            }
        }

        if (a.command == "quit" || a.command == "exit") break;
        if (a.command == "help") {
            std::cout << "  the chat:\n"
                         "  open <room> / view [room]  open a room\n"
                         "  send <room> <text>   send a message\n"
                         "  find <query> / search <query>  the message search (bodies, mxids, links)\n"
                         "  thread <room> [root]  the threads\n"
                         "  up / down [n] / top / bottom / scroll <n>  the viewport\n"
                         "  goto <event_id> | lastread | newest   jump to a place\n"
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
            continue;
        }
        if (asciiSettingsCommand(st, dbi, a)) continue;
        if (asciiCommandDispatch(st, dbi, a)) continue;
        if (a.command == "open" || a.command == "view") {
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            loadRoomIntoStateImpl(st, q);
            if (st.mobile) {
                st.mobileTab = 1;  // Element Classic: opening jumps to Chat
                st.scroll = 0;
            }
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        if (a.command == "thread") {
            if (!a.positional.empty() && a.positional[0] == "off") {
                st.rightPanel = 0;
                st.statusNote = "right panel: members";
                std::cout << drawFrameImpl(st) << std::flush;
                continue;
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
                continue;
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
                    continue;
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
                    continue;
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
            continue;
        }
        // ---- threads: threads across ALL rooms (the Element thread list) ----
        if (a.command == "threads") {
            st.rightPanel = 3;
            st.statusNote = "right panel: threads in all rooms";
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
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
            continue;
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
            continue;
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
            continue;
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
            continue;
        }
        if (a.command == "people" && st.mobile) {
            st.mobileTab = 2;
            st.scroll = 0;
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
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
            continue;
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
            continue;
        }
        if (a.command == "refresh") {
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        if (a.command == "attach" || a.command == "send-file") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: attach <room> <file> [--caption text]" << std::endl;
                continue;
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
            continue;
        }
        if (a.command == "send") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: send <room> <text>" << std::endl;
                continue;
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
            continue;
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
            continue;
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
            continue;
        }
        // ---- modredact on|off: whether the client may redact OTHER
        // users' messages (the moderation) — off by default. Your OWN
        // messages are always redactable. ----
        // ---- powerlevels: the full power-level structure (the custom
        // user levels, the event overrides, the defaults) ----
        if (a.command == "powerlevels" || a.command == "pl") {
            if (st.powerLevelsEvent.empty() || st.powerLevelsEvent.is_null()) {
                std::cout << "No m.room.power_levels in the cache for this room."
                          << std::endl;
                continue;
            }
            const auto& pl = st.powerLevelsEvent;
            std::string roomLabel = st.currentRoomId;
            for (const auto& r : st.rooms) {
                if (r.value("room_id", "") == st.currentRoomId) {
                    roomLabel = roomDisplayNameImpl(r);
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
            continue;
        }
        // ---- accounts / account: multi-account switching ----
        if (a.command == "accounts") {
            auto cliHandler = CommandRegistry::instance().findCli("accounts");
            if (cliHandler) cliHandler(a);
            continue;
        }
        if (a.command == "account" || a.command == "switch") {
            if (a.positional.empty()) {
                std::cout << "Usage: account <@user:server>  (see 'accounts')" << std::endl;
                continue;
            }
            if (!pcore::init()) { std::cout << "No session store." << std::endl; continue; }
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
            continue;
        }
        // ---- verify: SAS session verification (needs a session) ----
        if (a.command == "verify" || a.command == "verify-wait") {
            auto cliHandler = CommandRegistry::instance().findCli(a.command);
            if (!cliHandler) {
                std::cout << "verify not available in this build." << std::endl;
                continue;
            }
            cliHandler(a);
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- presence: fetch member presence (needs a session) ----
        if (a.command == "presence") {
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "presence needs a logged-in session." << std::endl;
                continue;
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
            continue;
        }
        // ---- moderation + room settings (delegate to the registry) ----
        if (a.command == "ban" || a.command == "kick" || a.command == "unban") {
            auto cliHandler = CommandRegistry::instance().findCli(a.command);
            if (!cliHandler) {
                std::cout << a.command << " not available in this build." << std::endl;
                continue;
            }
            cliHandler(a);
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- media: download + save/open a media event ----
        if (a.command == "media") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: media <room> <event_id> [--open] [--preview]" << std::endl;
                std::cout << "  --preview renders the image inline (ANSI half-blocks)"
                          << std::endl;
                continue;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomQ || id.find(roomQ) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            matrix::Event ev;
            if (!dbi.getEventById(a.positional[1], ev)) {
                std::cout << "Event not found in the cache: " << a.positional[1] << std::endl;
                continue;
            }
            // The plain "url" or the encrypted "file.url".
            std::string mxc;
            std::string key, iv, sha;
            if (ev.content.is_object()) {
                auto urlIt = ev.content.find("url");
                if (urlIt != ev.content.end() && urlIt->is_string()) mxc = urlIt->get<std::string>();
                auto fIt = ev.content.find("file");
                if (mxc.empty() && fIt != ev.content.end() && fIt->is_object()) {
                    auto fu = fIt->find("url");
                    if (fu != fIt->end() && fu->is_string()) mxc = fu->get<std::string>();
                    auto k = fIt->find("key");
                    if (k != fIt->end() && k->is_string()) key = k->get<std::string>();
                    auto ivv = fIt->find("iv");
                    if (ivv != fIt->end() && ivv->is_string()) iv = ivv->get<std::string>();
                    auto h = fIt->find("hashes");
                    if (h != fIt->end() && h->is_object()) {
                        auto s = h->find("sha256");
                        if (s != h->end() && s->is_string()) sha = s->get<std::string>();
                    }
                }
            }
            if (mxc.empty()) {
                std::cout << "No media url in the event." << std::endl;
                continue;
            }
            bool mediaSession = pcore::init() && pcore::loadSavedSession();
            std::vector<uint8_t> bytes;
            if (!mediaSession) {
                if (a.options.count("preview")) {
                    std::cout << "(no session — previewing the demo sample)"
                              << std::endl;
                } else {
                    std::cout << "media download needs a logged-in session."
                              << std::endl;
                }
            } else {
                auto& core = pcore::core();
                auto client = core.client;
                if (!key.empty()) {
                    auto r = client->downloadMediaEncrypted(mxc, key, iv, sha);
                    if (r.ok) bytes = r.data;
                } else {
                    auto r = client->downloadMedia(mxc, 0, 0);
                    if (r.ok) bytes = r.data;
                }
                if (bytes.empty()) {
                    std::cout << "Download failed; " << std::endl;
                }
            }
            // --preview: render the image inline in the terminal. The CLI has
            // no image library, so the rendering is delegated to a system
            // tool when one exists: chafa (best, ANSI true-color), jp2a
            // (ascii art) or img2txt (libcaca). Nothing is written to disk.
            if (a.options.count("preview")) {
                std::string tmpImg = "/tmp/matrixcli_preview.png";
                if (bytes.empty()) {
                    // No downloadable bytes (demo/local mxc): generate a
                    // sample image so the preview still demonstrates itself.
                    std::cout << "(demo mxc — rendering a sample image instead)"
                              << std::endl;
                    std::string gen = "magick -size 320x160 gradient:orange-red "
                        "-fill white -pointsize 20 -gravity center "
                        "-annotate 0 'demo image' '" + tmpImg
                        + "' 2>/dev/null || convert -size 320x160 "
                        "gradient:orange-red -fill white -pointsize 20 "
                        "-gravity center -annotate 0 'demo image' '" + tmpImg
                        + "' 2>/dev/null";
                    std::system(gen.c_str());
                } else {
                    std::ofstream pout(tmpImg, std::ios::binary);
                    pout.write(reinterpret_cast<const char*>(bytes.data()),
                               static_cast<std::streamsize>(bytes.size()));
                }
                int cols = std::max(30, terminalWidthImpl() - 4);
                int rows = std::max(10, cols / 3);
                std::string cmd;
                if (std::system("which chafa >/dev/null 2>&1") == 0) {
                    cmd = "chafa --format symbols --size " + std::to_string(cols) + "x"
                        + std::to_string(rows) + " '" + tmpImg + "' 2>/dev/null";
                } else if (std::system("which jp2a >/dev/null 2>&1") == 0) {
                    cmd = "jp2a --width=" + std::to_string(cols) + " '" + tmpImg
                        + "' 2>/dev/null";
                } else if (std::system("which img2txt >/dev/null 2>&1") == 0) {
                    cmd = "img2txt -W " + std::to_string(cols) + " -H "
                        + std::to_string(rows) + " '" + tmpImg + "' 2>/dev/null";
                } else {
                    std::cout << "(no image renderer found — install chafa for inline"
                                 " previews; the file is saved below instead)"
                              << std::endl;
                }
                if (!cmd.empty()) std::system(cmd.c_str());
                std::remove(tmpImg.c_str());
            }
            std::string fn = "media_" + a.positional[1].substr(0, 12) + ".bin";
            std::ofstream out(fn, std::ios::binary);
            if (!out) {
                std::cout << "Cannot write " << fn << std::endl;
                continue;
            }
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            out.close();
            st.statusNote = "media saved: " + fn + " (" + std::to_string(bytes.size()) + " bytes)";
            std::cout << "Saved " << bytes.size() << " bytes to " << fn << std::endl;
            if (a.options.count("open")) {
                std::string cmd = "xdg-open '" + fn + "' 2>/dev/null &";
                std::system(cmd.c_str());
            }
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- dump: export the room like Element Web ----

        // ---- profile: show a user's profile (display name, avatar) ----
        if (a.command == "profile") {
            if (a.positional.empty()) {
                std::cout << "Usage: profile <@user:server>  (see 'members' for the"
                             " full ids)" << std::endl;
                continue;
            }
            auto cliHandler = CommandRegistry::instance().findCli("profile");
            if (!cliHandler) {
                std::cout << "profile not available in this build." << std::endl;
                continue;
            }
            cliHandler(a);
            continue;
        }
        // ---- members: list the room's members with FULL ids ----
        if (a.command == "members") {
            // The user-list layout setting (a room query would never match
            // these words, so they are safe to intercept).
            if (a.positional.size() >= 1 &&
                (a.positional[0] == "horizontal" || a.positional[0] == "list" ||
                 a.positional[0] == "vertical" || a.positional[0] == "auto")) {
                std::string v = a.positional[0];
                if (v == "horizontal") st.membersMode = 1;
                else if (v == "list" || v == "vertical") st.membersMode = 2;
                else st.membersMode = 0;
                dbi.setSetting("members_mode", std::to_string(st.membersMode));
                st.statusNote = std::string("members ") +
                                (st.membersMode == 1 ? "horizontal" :
                                 st.membersMode == 2 ? "vertical list" : "auto");
                std::cout << drawFrameImpl(st) << std::flush;
                continue;
            }
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            std::string roomId = q;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == q || id.find(q) != std::string::npos ||
                    name == q || name.find(q) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            auto evs = st.db->getEvents(roomId, 500);
            std::vector<std::string> seen;
            for (const auto& ev : evs) {
                if (std::find(seen.begin(), seen.end(), ev.sender) == seen.end()) {
                    seen.push_back(ev.sender);
                }
            }
            for (const auto& m : seen) {
                auto pit = st.presence.find(m);
                std::string letter = pit != st.presence.end() ? "[" + pit->second + "] " : "";
                std::cout << "  " << letter << m << "  (profile <@user>)" << std::endl;
            }
            if (seen.empty()) std::cout << "(no members in the cache for " << roomId << ")"
                                        << std::endl;
            continue;
        }
        // ---- panel <left|center|right> <off|on|W> ----
        if (a.command == "panel") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: panel <left|right> <off|on|width>" << std::endl;
                continue;
            }
            std::string which = a.positional[0];
            std::string v = a.positional[1];
            int w = -1;
            if (v == "off") w = 0;
            else if (v == "on") w = -1;
            else { try { w = std::stoi(v); } catch (...) { w = -1; } }
            if (which == "left") st.leftPanelW = w;
            else if (which == "right") st.rightPanelW = w;
            else {
                std::cout << "Usage: panel <left|right> <off|on|width>" << std::endl;
                continue;
            }
            st.statusNote = std::string("panel ") + which + " = " + v;
            dbi.setSetting(which == "left" ? "panel_left" : "panel_right",
                           std::to_string(w));
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- panel auto on|off: size the panels to the content ----
        if (a.command == "panel" && a.positional.size() >= 2 &&
            a.positional[0] == "auto") {
            st.autoPanels = (a.positional[1] != "off" && a.positional[1] != "0");
            dbi.setSetting("panel_auto", st.autoPanels ? "1" : "0");
            st.statusNote = std::string("panel auto ") +
                            (st.autoPanels ? "on (sized to content)" : "off (fixed)");
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- emoji on|off: emoji glyphs or ASCII fallbacks ----
        if (a.command == "emoji") {
            if (a.positional.empty() || a.positional[0] == "on") st.showEmoji = true;
            else st.showEmoji = false;
            st.statusNote = std::string("emoji ") + (st.showEmoji ? "on" : "off (ASCII)");
            dbi.setSetting("emoji", st.showEmoji ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- images on|off: full image cards ----
        if (a.command == "images") {
            if (a.positional.empty() || a.positional[0] == "on") st.showImages = true;
            else st.showImages = false;
            st.statusNote = std::string("images ") + (st.showImages ? "full cards" : "compact");
            dbi.setSetting("images", st.showImages ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- raw <room> <event_id>: the event's raw JSON ----
        if (a.command == "raw") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: raw <room> <event_id>" << std::endl;
                continue;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : st.rooms) {
                std::string id = r.value("room_id", "");
                if (id == roomQ || id.find(roomQ) != std::string::npos) {
                    roomId = id;
                    break;
                }
            }
            matrix::Event ev;
            if (!dbi.getEventById(a.positional[1], ev)) {
                std::cout << "Event not found in the cache: " << a.positional[1]
                          << std::endl;
                continue;
            }
            nlohmann::json j;
            j["event_id"] = ev.event_id;
            j["room_id"] = roomId;
            j["sender"] = ev.sender;
            j["type"] = ev.type;
            j["origin_server_ts"] = ev.origin_server_ts;
            j["content"] = ev.content;
            std::cout << j.dump(2) << std::endl;
            continue;
        }
        // ---- about: version + ASCII logo ----
        if (a.command == "about") {
            printAbout(st.proxyLabel, st.accountLabel);
            continue;
        }
        // ---- ids on|off: show event ids next to the messages ----
        if (a.command == "ids") {
            if (a.positional.empty() || a.positional[0] == "on") st.showIds = true;
            else if (a.positional[0] == "off") st.showIds = false;
            else st.showIds = true;
            st.statusNote = std::string("event ids ") + (st.showIds ? "shown" : "hidden");
            dbi.setSetting("ids", st.showIds ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- time [full|sec|off]: message time with seconds ----
        if (a.command == "time") {
            if (a.positional.empty()) {
                std::cout << "Usage: time full | time sec (HH:MM:SS) | time off (HH:MM)"
                          << std::endl;
                continue;
            }
            std::string v = a.positional[0];
            if (v == "off") st.showSeconds = false;
            else st.showSeconds = true;  // full / sec / anything = seconds on
            st.statusNote = std::string("time ") + (st.showSeconds ? "HH:MM:SS" : "HH:MM");
            dbi.setSetting("time_full", st.showSeconds ? "1" : "0");
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- timeside left|right: the chat time on the left or right ----
        if (a.command == "timeside") {
            std::string v = a.positional.empty() ? "" : a.positional[0];
            if (v != "left" && v != "right") {
                std::cout << "Usage: timeside left | timeside right" << std::endl;
                continue;
            }
            st.timeRight = (v == "right");
            dbi.setSetting("time_side", v);
            st.statusNote = "time on the " + v;
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- msgline inline|newline: message next to or under the time ----
        if (a.command == "msgline") {
            std::string v = a.positional.empty() ? "" : a.positional[0];
            if (v != "inline" && v != "newline") {
                std::cout << "Usage: msgline inline | msgline newline" << std::endl;
                continue;
            }
            st.msgNewline = (v == "newline");
            dbi.setSetting("msg_line", v);
            st.statusNote = "message " + v;
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }

        // ---- agent <prompt>: the local coding agent (opencode-style) ----
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
                    continue;
                }
                dbi.setSetting("agent_trust", v);
            cfg.trust = v;
            agenttools::saveAgentConfig(cfg);
                st.statusNote = "agent trust: " + v;
                continue;
            }
            if (!a.positional.empty() &&
                (a.positional[0] == "allow" || a.positional[0] == "deny")) {
                if (a.positional.size() < 2) {
                    std::cout << "Usage: agent " << a.positional[0]
                              << " <command-prefix>" << std::endl;
                    continue;
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
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "config") {
                if (a.positional.size() < 3) {
                    std::cout << "Usage: agent config provider|endpoint|model|key <value>"
                              << std::endl;
                    continue;
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
                    continue;
                }
                st.statusNote = "agent " + k + " set";
                agenttools::saveAgentConfig(cfg);
                continue;
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
                    continue;
                }
                std::string act = a.positional[3];
                if (act != "allow" && act != "ask" && act != "deny") {
                    std::cout << "the action must be allow|ask|deny" << std::endl;
                    continue;
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
                continue;
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
                continue;
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
                continue;
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
                    continue;
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
                continue;
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
                continue;
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
                        continue;
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
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "undo") {
                int n = 1;
                if (a.positional.size() >= 2) {
                    try { n = std::stoi(a.positional[1]); } catch (...) {}
                }
                int removed = agenttools::undoLastTurns(agentHistory, n);
                st.statusNote = "undo: removed " + std::to_string(removed)
                              + " messages";
                continue;
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
                    continue;
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
                    continue;
                }
                agentHistory.resize(static_cast<size_t>(target + 1));
                agentHistory[static_cast<size_t>(target)].content = text;
                st.statusNote = "edited — the last answer is dropped";
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "title") {
                if (cfg.key.empty()) {
                    std::cout << "no API key — the title needs the LLM"
                              << std::endl;
                    continue;
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
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "lsp") {
                // agent lsp hover|def <file>:<line>:<col>
                if (a.positional.size() < 3) {
                    std::cout << "Usage: agent lsp hover|def <file>:<line>:<col>"
                              << std::endl;
                    continue;
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
                    continue;
                }
                std::cout << agenttools::lspQueryPublic(cfg, op, path, line,
                                                        col) << std::endl;
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "usage") {
                std::cout << "  " << agenttools::agentUsageLine() << std::endl;
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "providers") {
                for (const auto& p : agenttools::providerPresets()) {
                    std::cout << "  " << p.name << "  →  " << p.provider
                              << " " << p.endpoint << " / " << p.model
                              << std::endl;
                }
                std::cout << "  (agent config provider <name> applies one)"
                          << std::endl;
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "reasoning") {
                std::string v = a.positional.size() >= 2 ? a.positional[1] : "";
                if (v != "low" && v != "medium" && v != "high") {
                    std::cout << "Usage: agent reasoning low|medium|high"
                              << std::endl;
                    continue;
                }
                cfg.reasoning = v;
                agenttools::saveAgentConfig(cfg);
                st.statusNote = "reasoning: " + v;
                continue;
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
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "proxy") {
                std::string v = a.positional.size() >= 2 ? a.positional[1] : "";
                if (v == "off") v = "";
                dbi.setSetting("agent_proxy", v);
            cfg.proxy = v;
            agenttools::saveAgentConfig(cfg);
                st.statusNote = v.empty() ? "agent proxy: off"
                                          : "agent proxy: " + v;
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "sandbox") {
                std::string v = a.positional.size() >= 2 ? a.positional[1] : "";
                if (v != "off" && v != "bwrap") {
                    std::cout << "Usage: agent sandbox off|bwrap" << std::endl;
                    continue;
                }
                dbi.setSetting("agent_sandbox", v);
            cfg.sandbox = v;
            agenttools::saveAgentConfig(cfg);
                st.statusNote = "agent sandbox: " + v;
                continue;
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
                continue;
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
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "subgoal") {
                std::string arg;
                for (size_t i = 1; i < a.positional.size(); ++i) {
                    arg += (arg.empty() ? "" : " ") + a.positional[i];
                }
                if (cfg.goal.goal.empty()) {
                    std::cout << "  no active goal — set one with 'agent goal'"
                              << std::endl;
                    continue;
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
                continue;
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
                continue;
            }
            if (!a.positional.empty() && a.positional[0] == "reset") {
                agentHistory.clear();
                st.statusNote = "agent history cleared";
                continue;
            }
            if (cfg.key.empty()) {
                std::cout << "No API key: set it with 'agent config key <key>',"
                             " 'agent config provider openai|anthropic' or the"
                             " OPENAI_API_KEY / ANTHROPIC_API_KEY env vars."
                          << std::endl;
                continue;
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
                continue;
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
                continue;
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
                if (!pr.ok) continue;
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
                continue;
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
            continue;
        }
        // ---- reply: send a reply to a message ----
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
                continue;
            }
            auto cliHandler = CommandRegistry::instance().findCli("reply");
            if (!cliHandler) {
                std::cout << "reply not available in this build." << std::endl;
                continue;
            }
            cliHandler(a);
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- jump: view the room starting from a date (like Element) ----
        if (a.command == "jump" || a.command == "date") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: jump <room> <YYYY-MM-DD> [--server]" << std::endl;
                std::cout << "  --server walks the server history (full room);"
                             " default = the cache" << std::endl;
                continue;
            }
            int64_t dayMs = parseDayMsImpl(a.positional[1]);
            if (dayMs < 0) {
                std::cout << "Bad date '" << a.positional[1] << "' (use YYYY-MM-DD)"
                          << std::endl;
                continue;
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
            continue;
        }
        // ---- invite: invite a user into the room ----
        if (a.command == "invite") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: invite <room> <@user>" << std::endl;
                continue;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "invite needs a logged-in session." << std::endl;
                continue;
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
            continue;
        }
        // ---- create-room / create-space ----
        if (a.command == "create-room" || a.command == "create-space") {
            if (a.positional.empty()) {
                std::cout << "Usage: create-room <name> [--topic T] [--encrypted]"
                             " [--public] [--invite @u1,@u2]"
                          << std::endl;
                continue;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "create-room needs a logged-in session." << std::endl;
                continue;
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
                continue;
            }
            std::string roomId = r.data;
            std::string kind = a.command == "create-space" ? "space" : "room";
            st.statusNote = kind + " created: " + a.positional[0] + " (" + roomId + ")";
            std::cout << "Created " << kind << ": " << a.positional[0] << " (" << roomId
                      << ")" << std::endl;
            st.rooms = dbi.listRooms();
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- add-to-space: make a room a child of a space ----
        if (a.command == "add-to-space") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: add-to-space <room> <space>" << std::endl;
                continue;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "add-to-space needs a logged-in session." << std::endl;
                continue;
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
            continue;
        }
        // ---- pin / unpin ----
        if ((a.command == "pin" || a.command == "unpin") &&
            !(a.positional.size() >= 2 && a.positional[0].size() == 5 &&
              a.positional[0][2] == ':')) {
            if (a.positional.size() < 2) {
                std::cout << "Usage: " << a.command << " <room> <event_id> |"
                             " pin <HH:MM> <[nick]>" << std::endl;
                continue;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << a.command << " needs a logged-in session." << std::endl;
                continue;
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
            continue;
        }
        // ---- search filters: --sender, --since, --until ----

        // ---- goto: jump the chat viewport to an event ----

        if (a.command == "newest") {
            st.focusEvent.clear();
            st.scroll = 1 << 30;  // clamped to the bottom in drawFrame
            st.statusNote = "back to the latest messages";
            if (st.mobile) st.mobileTab = 1;
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- threads bottom on|off: the thread list in the right panel ----
        if (a.command == "threads" && a.positional.size() >= 2 &&
            a.positional[0] == "bottom") {
            st.showThreadsBottom = (a.positional[1] != "off");
            dbi.setSetting("threads_bottom", st.showThreadsBottom ? "1" : "0");
            st.statusNote = std::string("thread list in the right panel ") +
                            (st.showThreadsBottom ? "on" : "off");
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
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
            continue;
        }
        // ---- widths: custom per-character widths (fix the pipes) ----
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
                continue;
            }
            if (a.positional[0] == "reset") {
                g_widthOverrides.clear();
                dbi.setSetting("widths", "");
                st.statusNote = "custom widths cleared";
                std::cout << drawFrameImpl(st) << std::flush;
                continue;
            }
            if (a.positional.size() < 2) {
                std::cout << "Usage: widths <char> <1|2>" << std::endl;
                continue;
            }
            uint32_t cp = utf8FirstCp(a.positional[0]);
            int wd = 0;
            try { wd = std::stoi(a.positional[1]); } catch (...) {}
            if (cp == 0 || (wd != 1 && wd != 2)) {
                std::cout << "Usage: widths <char> <1|2>" << std::endl;
                continue;
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
            continue;
        }
        // ---- permalink <room> <event_id>: the matrix.to link with via ----
        if (a.command == "permalink") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: permalink <room> <event_id>" << std::endl;
                continue;
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
                continue;
            }
            std::cout << "https://matrix.to/#/" << roomId << "/" << ev.event_id
                      << viaSuffix(&dbi, roomId, st.viaLimit) << std::endl;
            continue;
        }
        // ---- via <n>: the via argument count in permalinks (0 = all) ----
        if (a.command == "via") {
            int v = 3;
            if (a.positional.empty()) {
                std::cout << "via: " << (st.viaLimit == 0 ? "unlimited (all servers)"
                                                          : std::to_string(st.viaLimit))
                          << "  (via <n> | via 0 = no limit)" << std::endl;
                continue;
            }
            try { v = std::stoi(a.positional[0]); } catch (...) { v = -1; }
            if (v < 0) {
                std::cout << "Usage: via <n> | via 0 (unlimited)" << std::endl;
                continue;
            }
            st.viaLimit = v;
            dbi.setSetting("via_limit", std::to_string(v));
            st.statusNote = std::string("permalinks via: ") +
                            (v == 0 ? "unlimited (all servers)" : std::to_string(v));
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
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
            continue;
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
            continue;
        }
        // ---- mute <room> on|off: no indicators, just stay ----
        if (a.command == "mute") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: mute <room> on|off" << std::endl;
                continue;
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
            continue;
        }
        // ---- timezone <N>: hours offset for all displayed times ----
        if (a.command == "timezone") {
            if (a.positional.empty()) {
                std::cout << "timezone: " << (st.tzOffset >= 0 ? "+" : "")
                          << st.tzOffset << "h  (timezone <N> | timezone 0)"
                          << std::endl;
                continue;
            }
            try { st.tzOffset = std::stoi(a.positional[0]); } catch (...) { st.tzOffset = 0; }
            dbi.setSetting("tz_offset", std::to_string(st.tzOffset));
            st.statusNote = std::string("timezone UTC") + (st.tzOffset >= 0 ? "+" : "")
                          + std::to_string(st.tzOffset);
            std::cout << drawFrameImpl(st) << std::flush;
            continue;
        }
        // ---- nick <room> <@user> <name>: per-room display names ----
        if (a.command == "nick") {
            if (a.positional.size() < 3) {
                std::cout << "Usage: nick <room> <@user> <name>" << std::endl;
                continue;
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
            continue;
        }
        // ---- avatar <room> <url>: per-room avatar (shown in info) ----
        if (a.command == "avatar") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: avatar <room> <mxc|url>" << std::endl;
                continue;
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
            continue;
        }
        // ---- color <@user> <color|off>: custom nickname highlight ----
        if (a.command == "color") {
            if (a.positional.empty()) {
                std::cout << "Usage: color <@user> <red|green|yellow|blue|magenta|cyan|off>"
                          << std::endl;
                continue;
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
            continue;
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
            continue;
        }
        // ---- reply/react/thread/pin/copy "HH:MM [nick]" — find the last
        // matching message and act on it ----
replyRef:
        if (a.command == "reply" || a.command == "react" ||
            a.command == "thread" || a.command == "pin" ||
            a.command == "copy") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: " << a.command
                          << " <HH:MM> <[nick]> [text|emoji]" << std::endl;
                continue;
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
                continue;
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
                continue;
            }
            std::string roomId = st.currentRoomId;
            int64_t nowTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (a.command == "copy") {
                std::cout << "https://matrix.to/#/" << roomId << "/"
                          << target->event_id
                          << viaSuffix(&dbi, roomId, st.viaLimit) << std::endl;
                continue;
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
                st.statusNote = "pinned " + target->event_id;
                std::cout << drawFrameImpl(st) << std::flush;
                continue;
            }
            if (a.positional.size() < 3) {
                std::cout << "Usage: " << a.command << " <HH:MM> <[nick]> <text>"
                          << std::endl;
                continue;
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
            continue;
        }
        // ---- star <room> on|off: anchor the room to the top of the list ----
        if (a.command == "star" || a.command == "anchor") {
            if (a.positional.empty()) {
                std::cout << "Usage: " << a.command << " <room> on|off" << std::endl;
                continue;
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
            continue;
        }
        // ---- roomname <room> <name> / topic <room> <text>: like Element ----
        if (a.command == "roomname" || a.command == "topic") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: " << a.command << " <room> <value>"
                          << std::endl;
                continue;
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
            continue;
        }
        // ---- power <room>: the power levels; power <room> <@user> <level>;
        // power <room> default <level> (send permission) ----
        if (a.command == "power") {
            if (a.positional.empty()) {
                std::cout << "Usage: power <room> [<@user> <level>|default <level>]"
                          << std::endl;
                continue;
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
                continue;
            }
            int level = 0;
            try { level = std::stoi(a.positional[2]); } catch (...) {
                std::cout << "Usage: power <room> <@user> <level>"
                          << std::endl;
                continue;
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
            continue;
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
                                      << clip(eventBodyImpl(target), 40) << "]";
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
            continue;
        }
        // ---- voice: record (arecord/parec) + send as m.audio ----
        if (a.command == "voice") {
            if (a.positional.empty()) {
                std::cout << "Usage: voice <room> [--seconds N] [--out file]" << std::endl;
                continue;
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
                continue;
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
            continue;
        }
        // ---- sticker: send an m.sticker ----
        if (a.command == "sticker") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: sticker <room> <name> [--url mxc]" << std::endl;
                continue;
            }
            if (!(pcore::init() && pcore::loadSavedSession())) {
                std::cout << "sticker needs a logged-in session." << std::endl;
                continue;
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
            continue;
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
            if (!st.messages.empty()) last = eventBodyImpl(st.messages.back());
            std::string cmd = "notify-send 'progressive-cli: " + rname + "' '" + clip(last, 80)
                            + "' 2>/dev/null || echo -e '\a'";
            std::system(cmd.c_str());
            st.statusNote = "notified for " + rname;
            continue;
        }
        std::cout << "Unknown command '" << a.command << "' — type 'help'.\n";
    }
    std::cout << "Bye!" << std::endl;
    return 0;
}

// Declared in main.cpp (the real media-upload send).
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
}

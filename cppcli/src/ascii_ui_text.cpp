// src/ascii_ui_text.cpp — the text/width helpers of the ASCII UI
// (terminalWidthImpl .. roomLastEvent). Split out of ascii_ui.cpp so
// no translation unit stays over ~1000 lines. Split: brace-matched.
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
    // pipes. The emoji blocks render TWO cells there like every terminal.
    // (The old "👑 🛡 🗳 📥 are narrow" overrides came from byte-column
    // checks, not real rendering — they shifted the header/panel pipes.)
    if (cp == 0x1F451 || cp == 0x1F6E1) return 1;   // 👑 🛡 power badges are narrow
    if (cp == 0x1F5F3) return 1;                    // 🗳 ballot box is narrow
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
// Wrap to a display width; the FIRST line may use a wider budget
// (firstW >= 0) — the message rows carry the time and a hint on line
// one, which can fill the panel where their continuations cannot.
std::vector<std::string> wrapTextImpl(const std::string& s, int width,
                                      int firstW) {
    std::vector<std::string> lines;
    std::string cur;
    std::string word;
    auto lineWidth = [&]() {
        return lines.empty() && firstW >= 0 ? firstW : width;
    };
    auto flushWord = [&]() {
        if (word.empty()) return;
        int wd = displayWidth(word);
        int sep = word.front() == ' ' ? 0 : 1;
        if (!cur.empty() && displayWidth(cur) + sep + wd > lineWidth()) {
            lines.push_back(cur);
            cur.clear();
        }
        if (!cur.empty() && word.front() != ' ') cur += " ";
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
            if (c == ' ' && cur.empty() && word.empty()) {
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
            } else if (c == ' ') {
                if (!cur.empty() && displayWidth(cur) + 1 > lineWidth()) {
                    lines.push_back(cur);
                    cur.clear();
                } else {
                    word += ' ';  // keep the separator (double spaces stay)
                }
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
        if (displayWidth(word) >= lineWidth()) {  // hard-split overlong words
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

std::string roomDisplayNameImpl(const UiState& st, const nlohmann::json& r) {
    std::string id = r.value("room_id", "");
    std::string name = r.value("name", "");
    std::string alias = r.value("canonical_alias", "");
    if (st.roomNames) {
        // Names mode: the human title first, then the alias as a fallback.
        if (!name.empty()) return name;
        if (!alias.empty()) return alias;
        return id;
    }
    // Aliases mode (default): the #handle first; legacy rows stored the
    // alias in the name column ("#general"), so that shows too.
    if (!alias.empty()) return alias;
    if (!name.empty() && name[0] == '#') return name;
    if (!name.empty()) return name;
    return id;
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

// The timestamp of a room's last MESSAGE event (for activity sorting
// and the room-list times; 0 = none). State/pin events — the newest
// entries — must not decide recency.
int64_t roomLastTs(db::Database* db, const std::string& roomId) {
    if (!db) return 0;
    auto evs = db->getEvents(roomId, 30);
    for (const auto& e : evs) {
        if (e.type == "m.room.message" || e.type == "m.sticker")
            return e.origin_server_ts;
    }
    if (!evs.empty()) return evs.front().origin_server_ts;
    return 0;
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

} // namespace matrixcli

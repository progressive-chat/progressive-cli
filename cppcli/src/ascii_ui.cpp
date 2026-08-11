// src/ascii_ui.cpp — ASCII-drawn client interface for the CLI (not the TUI).
//
// `matrixcli ui` draws a chat-client-like layout with plain characters:
// a header, a left panel with the room list, the open room's messages in
// the center and the member list on the right, panels separated by pipes.
// It is a REPL: every command executes and the whole frame is redrawn —
// no auto-updates, no raw terminal mode (works in any terminal, scrolls
// like a normal CLI program).
#include "ascii_ui.hpp"
#include "commands.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/logger.hpp"
#include "cli/args.hpp"
#include "pcore.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
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

int terminalWidth() {
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) {
        return static_cast<int>(ws.ws_col);
    }
#endif
    return 100;
}

// Approximate terminal display width of one UTF-8 codepoint: 2 for CJK,
// emoji and misc symbols, 0 for combining marks, else 1.
int cpWidth(uint32_t cp) {
    if (cp >= 0x0300 && cp <= 0x036F) return 0;      // combining
    if (cp == 0xFE0F || cp == 0xFE0E) return 0;      // variation selectors
    if (cp >= 0x1100 && cp <= 0x115F) return 2;
    if (cp >= 0x2E80 && cp <= 0xA4CF) return 2;      // CJK
    if (cp >= 0xAC00 && cp <= 0xD7A3) return 2;
    if (cp >= 0xF900 && cp <= 0xFAFF) return 2;
    if (cp >= 0xFE30 && cp <= 0xFE4F) return 2;
    if (cp >= 0xFF00 && cp <= 0xFF60) return 2;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2;
    if (cp >= 0x2190 && cp <= 0x21FF) return 1;      // text arrows ←↑→ are 1
    if (cp >= 0x2713 && cp <= 0x2717) return 1;      // ✓ ✗ text marks are 1
    if (cp >= 0x2600 && cp <= 0x27BF) return 2;      // ⤷❤📌 etc.
    if (cp >= 0x2B00 && cp <= 0x2BFF) return 2;
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
        uint32_t cp = 0;
        size_t len = 0;
        unsigned char c = static_cast<unsigned char>(s[i]);
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

// Pad to a display width (cells, not bytes — keeps the | columns aligned
// with emoji/CJK in the rows).
std::string pad(const std::string& s, int width) {
    std::string out = clip(s, width);
    int w = displayWidth(out);
    if (w < width) out.append(static_cast<size_t>(width - w), ' ');
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
std::string eventBody(const matrix::Event& ev) {
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
    auto thr = rel->find("m.thread");
    if (thr != rel->end() && thr->is_object()) {
        auto eid = thr->find("event_id");
        if (eid != thr->end() && eid->is_string()) {
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
int64_t parseDayMs(const std::string& s) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -1;
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
    t.tm_hour = 12;
    auto tt = std::mktime(&t);
    return tt < 0 ? -1 : static_cast<int64_t>(tt) * 1000;
}

// Short preview of another event's body (for reply/thread indentation).
std::string eventPreview(db::Database* db, const std::string& roomId,
                         const std::string& eventId) {
    matrix::Event ev;
    if (!db || eventId.empty() || !db->getEventById(eventId, ev)) return "";
    std::string b = eventBody(ev);
    return clip(b, 24);
}

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

std::string roomDisplayName(const nlohmann::json& r) {
    std::string id = r.value("room_id", "");
    std::string name = r.value("name", "");
    if (name.empty()) name = id;
    return name;
}

// Message count comes from the DB (listRooms json carries no count).
int roomMessageCount(db::Database* db, const std::string& roomId) {
    return db ? db->getEventCount(roomId) : 0;
}

std::string senderShort(const std::string& sender) {
    std::string s = sender;
    if (!s.empty() && s[0] == '@') {
        auto colon = s.find(':');
        if (colon != std::string::npos) s = s.substr(1, colon - 1);
        else s = s.substr(1);
    }
    return s;
}

// The last message of a room as a preview row ("alice: Welcome!") like
// Element's room list. Message events only; joins/lefts show a short
// summary; rooms with nothing at all get an empty string.
std::string roomLastMsg(db::Database* db, const std::string& roomId) {
    if (!db) return "";
    auto evs = db->getEvents(roomId, 1);
    if (evs.empty()) return "";
    const matrix::Event& ev = evs.front();
    std::string preview;
    if (ev.type == "m.room.message" || ev.type == "m.sticker") {
        preview = eventBody(ev);
        std::string mt = ev.content.value("msgtype", "");
        if (mt == "m.file") preview = "📄 " + preview;
        else if (mt == "m.audio") preview = "🎵 " + preview;
        else if (mt == "m.image") preview = "🖼 " + preview;
        else if (mt == "m.poll.start") {
            auto q = ev.content.find("question");
            if (q != ev.content.end() && q->is_object()) {
                auto t = q->find("text");
                if (t != q->end() && t->is_string()) preview = t->get<std::string>();
            }
            preview = "⭕ poll: " + preview;
        }
        else if (ev.type == "m.sticker") preview = "👻 " + preview;
    } else if (ev.type == "m.room.member") {
        std::string m = ev.content.value("membership", "");
        preview = (m == "leave" || m == "ban") ? "❌ left"
                                               : "✓ joined";
    }
    if (preview.empty()) return "";
    return senderShort(ev.sender) + ": " + preview;
}

// The time of the last event in a room, for the room-list rows: today's
// events show HH:MM (HH:MM:SS with the "time full" setting), older ones
// show the date as MM-DD (Element Classic style).
std::string roomLastTime(db::Database* db, const std::string& roomId, bool seconds) {
    if (!db) return "";
    auto evs = db->getEvents(roomId, 1);
    if (evs.empty()) return "";
    std::time_t t = static_cast<std::time_t>(evs.front().origin_server_ts / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{};
    localtime_r(&nowT, &nowTm);
    char buf[16];
    if (tm.tm_year == nowTm.tm_year && tm.tm_yday == nowTm.tm_yday) {
        std::snprintf(buf, sizeof(buf), seconds ? "%02d:%02d:%02d" : "%02d:%02d",
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        std::snprintf(buf, sizeof(buf), "%02d-%02d", tm.tm_mon + 1, tm.tm_mday);
    }
    return buf;
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
                std::string preview = eventBody(ev);
                pill += " \u00b7 " + senderShort(ev.sender) + ": " + clip(preview, 24);
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


// ---- the frame ----

struct UiState {
    db::Database* db = nullptr;
    std::vector<nlohmann::json> rooms;   // listRooms()
    std::string currentRoomId;
    std::vector<matrix::Event> messages; // getEvents(currentRoom)
    std::vector<std::string> members;    // unique senders in the room
    int limit = 25;
    int scroll = 0;                      // viewport offset (rows)
    std::string accountLabel;            // e.g. "bob@matrix.org" or "demo (offline)"
    std::string proxyLabel;              // "on (socks5h ...)" or "off"
    std::string roomFilter;              // find/space filter for the left panel
    std::string statusNote;              // last action's summary (dump etc.)
    bool staticFrame = false;            // --static: one-shot frame
    bool mobile = false;                 // smartphone: stacked sections
    int invites = 0;                     // open invites for the logged-in user
    std::string activeSpace;             // "" = all rooms; else a space id
    int mobileTab = 0;                   // 0=Rooms 1=Chat 2=People (bottom nav)
    int limitRows = 0;                   // settings "rows <n>": 0 = fit terminal
    std::map<std::string, std::string> presence; // member -> О/А/Ф letters
    // Right panel mode: 0 = members, 1 = room thread list, 2 = one thread,
    // 3 = threads across all rooms (Element-style thread panel).
    bool showIds = false;       // show event ids next to the messages
    bool showSeconds = false;   // HH:MM:SS instead of HH:MM
    bool showImages = false;    // full image cards (default: compact marker)
    bool showEmoji = true;      // emoji glyphs; off = ASCII fallbacks
    int leftPanelW = -1;        // -1 = default width, 0 = hidden
    int rightPanelW = -1;       // -1 = default width, 0 = hidden
    std::map<std::string, int> powerLevels;  // member -> power level
    std::unordered_set<std::string> redactedIds;  // events that were redacted
    std::map<std::string, std::string> receipts;  // eventId -> "a b" readers
    int rightPanel = 0;
    std::string threadRoomId;   // for the room thread list
    std::string threadRootId;   // for the single-thread view
    std::vector<matrix::Event> threadReplies;  // replies of threadRootId
};

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
int contentRows(const UiState& st) {
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
        std::string name = senderShort(m);
        if (name.empty()) continue;
        // The shortest prefix not shared with any other member.
        std::string prefix;
        for (size_t len = 1; len <= name.size(); ++len) {
            prefix = name.substr(0, len);
            bool unique = true;
            for (const auto& o : members) {
                if (o == m) continue;
                if (senderShort(o).compare(0, prefix.size(), prefix) == 0) {
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

void loadRoomIntoState(UiState& st, const std::string& query) {
    st.currentRoomId.clear();
    for (const auto& r : st.rooms) {
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        if (id == query || name == query ||
            (!query.empty() && (name.find(query) == 0 ||
                                 id.find(query) != std::string::npos))) {
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
    st.scroll = 0;  // clamped to the bottom in drawFrame
    st.members.clear();
    st.powerLevels.clear();
    st.redactedIds.clear();
    for (const auto& ev : st.messages) {
        if (std::find(st.members.begin(), st.members.end(), ev.sender) ==
            st.members.end()) {
            st.members.push_back(ev.sender);
        }
        if (ev.type == "m.room.power_levels" && ev.content.is_object()) {
            auto users = ev.content.find("users");
            if (users != ev.content.end() && users->is_object()) {
                for (auto& [uid, lvl] : users->items()) {
                    if (lvl.is_number()) st.powerLevels[uid] = lvl.get<int>();
                }
            }
        }
        if (!ev.redacts.empty()) st.redactedIds.insert(ev.redacts);
    }
    // Read receipts (approximation): a message is read by the members whose
    // own message comes AFTER it in the timeline.
    st.receipts.clear();
    auto abbrev = minimalUniqueNames(st.members);
    for (const auto& ev : st.messages) {
        std::string readers;
        for (const auto& ev2 : st.messages) {
            if (ev2.origin_server_ts > ev.origin_server_ts &&
                ev2.sender != ev.sender) {
                auto it = abbrev.find(ev2.sender);
                if (it != abbrev.end()) {
                    if (!readers.empty()) readers += " ";
                    readers += it->second;
                }
            }
        }
        st.receipts[ev.event_id] = readers;
    }
}

std::string drawFrame(const UiState& st) {
    int W = terminalWidth();
    int leftW = st.leftPanelW >= 0 ? st.leftPanelW : std::max(22, W / 5);
    int rightW = st.rightPanelW >= 0 ? st.rightPanelW : std::max(16, W / 6);
    if (st.leftPanelW == 0) leftW = 0;
    if (st.rightPanelW == 0) rightW = 0;
    int centerW = std::max(20, W - leftW - rightW - 2);

    std::string roomName = "No room selected";
    for (const auto& r : st.rooms) {
        if (r.value("room_id", "") == st.currentRoomId) {
            roomName = roomDisplayName(r);
            break;
        }
    }

    // Header
    std::string out;
    std::string header = " " + roomName + " ";
    int headerFill = W - static_cast<int>(header.size());
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
    std::string headRoom = " " + roomName;
    if (static_cast<int>(headRoom.size()) > centerW - 1) headRoom = headRoom.substr(0, centerW - 1);
    const char* PIPE = "\x1b[90m";  // dim grey for the panel pipes
    const char* X = "\x1b[0m";
    if (!st.mobile) {
        out += pad(leftHeader, static_cast<size_t>(leftW)) + PIPE + "|" + X
             + pad(headRoom, static_cast<size_t>(centerW)) + PIPE + "|" + X
             + " Members" + std::string(std::max(0, rightW - 8), ' ') + "\n";
        out += PIPE + repeat('-', leftW) + "+" + repeat('-', centerW) + "+"
             + repeat('-', rightW) + X + "\n";
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
    std::vector<const nlohmann::json*> visible;
    for (const auto& r : st.rooms) {
        if (r.value("is_space", false)) continue;
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
    {
        auto renderRow = [&](const matrix::Event& ev) -> std::string {
            std::string center;
            std::string body = eventBodyRaw(ev);
            // Membership events render as "joined/left the room" rows.
            if (ev.type == "m.room.member" && ev.content.is_object()) {
                auto m = ev.content.find("membership");
                if (m != ev.content.end() && m->is_string()) {
                    std::string ms = m->get<std::string>();
                    std::string who = senderShort(ev.sender);
                    if (ms == "join") center = "[" + who + "] joined the room";
                    else if (ms == "leave") center = "[" + who + "] left the room";
                    else if (ms == "invite") center = "[" + who + "] was invited";
                    else if (ms == "ban") center = "[" + who + "] was banned";
                    else center = "[" + who + "] membership: " + ms;
                }
                if (center.empty()) center = "[" + senderShort(ev.sender) + "] (member event)";
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
                    center = "[" + senderShort(ev.sender) + "] "
                           + (st.showEmoji ? "\u2b55 poll: " : "[poll] ")
                           + (qtext.empty() ? "?" : qtext);
                } else if (mt == "m.sticker") {
                    center = "[" + senderShort(ev.sender) + "] "
                           + (st.showEmoji ? "\u2b1c sticker: " : "[sticker] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.audio") {
                    center = "[" + senderShort(ev.sender) + "] "
                           + (st.showEmoji ? "\u266a audio: " : "[audio] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.file") {
                    center = "[" + senderShort(ev.sender) + "] "
                           + (st.showEmoji ? "\xf0\x9f\x93\x84 " : "[file] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.video") {
                    center = "[" + senderShort(ev.sender) + "] "
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
                    center = "[" + senderShort(ev.sender) + "] " + imgPrefix
                           + "\x1b[36m" + body + "\x1b[0m" + dims;
                }
            }
            std::string thr = eventThreadRoot(ev);
            std::string rep = eventReplyTo(ev);
            if (center.empty() && !thr.empty()) {
                std::string preview = eventPreview(st.db, st.currentRoomId, thr);
                center = "[" + senderShort(ev.sender) + "] \u2937 " + body
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
                    std::string preview = eventBody(prev);
                    if (preview.empty()) break;
                    chain += std::string(lvl, ' ') + "> [" + senderShort(prev.sender)
                           + "] " + clip(preview, std::max(20, centerW - 26)) + "\n";
                    auto rel = prev.content.find("m.relates_to");
                    if (rel == prev.content.end() || !rel->is_object()) break;
                    auto ir = rel->find("m.in_reply_to");
                    if (ir == rel->end() || !ir->is_object()) break;
                    auto eid = ir->find("event_id");
                    if (eid == ir->end() || !eid->is_string()) break;
                    cur = eid->get<std::string>();
                }
                center = "[" + senderShort(ev.sender) + "] " + body
                       + (chain.empty() ? "" : "\n" + chain);
            } else if (center.empty()) {
                center = "[" + senderShort(ev.sender) + "] "
                       + highlightUrls(renderPermalinks(highlightMentions(body),
                                                        st.rooms, st.db));
                int rc = 0;
                for (const auto& ev2 : st.messages) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0) center += "  \u2937(" + std::to_string(rc) + ")";
            }
            // Redaction events render as system lines.
            if (ev.type == "m.room.redaction") {
                return "\xf0\x9f\x97\x91 " + senderShort(ev.sender)
                     + " deleted a message";  // 🗑
            }
            // Redacted targets get the (deleted) marker.
            if (st.redactedIds.count(ev.event_id)) {
                center += "  (\xf0\x9f\x97\x91 deleted)";
            }
            // Poll response counts under the poll.
            if (!center.empty() &&
                center.find("poll:") != std::string::npos && ev.content.is_object()) {
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
                        vstr += k + ": " + std::to_string(n);
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
                std::string sep = "── " + label + " ──";
                if (static_cast<int>(sep.size()) < centerW) {
                    sep = std::string((centerW - static_cast<int>(sep.size())) / 2, ' ') + sep;
                }
                centerRows.push_back(sep);
                prevDay = day;
            }
            std::string row = renderRow(ev);
            if (!row.empty()) {
                auto rIt = st.receipts.find(ev.event_id);
                if (rIt != st.receipts.end() && !rIt->second.empty()) {
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
                std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000);
                std::tm tm{};
                localtime_r(&t, &tm);
                char buf[16];
                if (st.showSeconds) {
                    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                                  tm.tm_hour, tm.tm_min, tm.tm_sec);
                } else {
                    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                                  tm.tm_hour, tm.tm_min);
                }
                std::string first = buf + std::string(" ") + row;
                if (st.showIds) {
                    std::string shortId = ev.event_id.substr(0, 10);
                    if (!shortId.empty()) {
                        first += "  \u2039" + shortId + "\u203a";
                    }
                }
                // Multiline bodies: split into one row per line, the
                // continuation lines indented under the first.
                std::vector<std::string> lines;
                std::string cur;
                std::istringstream iss(first);
                while (std::getline(iss, cur, '\n')) lines.push_back(cur);
                for (size_t li = 0; li < lines.size(); ++li) {
                    if (li == 0) {
                        centerRows.push_back(lines[0]);
                    } else {
                        centerRows.push_back(std::string(8, ' ') + lines[li]);
                    }
                }
            }
        }
    }
    // Right panel content per mode: members | room thread list | one thread
    // | threads across all rooms (Element-style).
    std::vector<std::string> rightRows;
    if (st.rightPanel == 0) {
        for (const auto& mem : st.members) {
            std::string m = senderShort(mem);
            auto pit = st.presence.find(mem);
            if (pit != st.presence.end() && !pit->second.empty()) {
                // O online = green, A away = yellow, F offline = red.
                const char* pc = "\x1b[32m";
                if (pit->second == "A") pc = "\x1b[33m";
                else if (pit->second == "F") pc = "\x1b[31m";
                m = std::string(pc) + "[" + pit->second + "]"
                  + "\x1b[0m " + m;
            }
            // Power-level badges: 👑 admin (100+), 🛡 mod (50+).
            auto pl = st.powerLevels.find(mem);
            if (pl != st.powerLevels.end()) {
                if (pl->second >= 100) m = "\xf0\x9f\x91\x91 " + m;  // 👑
                else if (pl->second >= 50) m = "\xf0\x9f\x9b\xa1 " + m;  // 🛡
            }
            rightRows.push_back(m);
        }
    } else if (st.rightPanel == 1) {
        // The room's threads: the roots with their reply counts.
        auto evs = st.db->getEvents(st.threadRoomId, 300);
        for (const auto& ev : evs) {
            int rc = 0;
            for (const auto& ev2 : evs) {
                if (eventThreadRoot(ev2) == ev.event_id) rc++;
            }
            if (rc > 0) {
                rightRows.push_back("\u2937 " + clip(eventBody(ev), 24) + " (" +
                                    std::to_string(rc) + ")");
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no threads in this room)");
    } else if (st.rightPanel == 2) {
        // One thread: the root + its replies (Element's thread panel).
        if (!st.threadRootId.empty()) {
            for (const auto& ev : st.messages) {
                if (ev.event_id == st.threadRootId) {
                    rightRows.push_back("[" + senderShort(ev.sender) + "] "
                                        + clip(eventBody(ev), 30));
                }
            }
            for (const auto& ev : st.threadReplies) {
                rightRows.push_back("  \u2937 [" + senderShort(ev.sender) + "] "
                                    + clip(eventBody(ev), 28));
            }
        }
        if (rightRows.empty()) rightRows.push_back("(thread is empty)");
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
                                        + clip(eventBody(ev), 16) + " (" +
                                        std::to_string(rc) + ")");
                    shown++;
                }
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no threads anywhere)");
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
            for (const auto* r : visible) {
                std::string rid = r->value("room_id", "");
                std::string mark = rid == st.currentRoomId ? "*" : " ";
                std::string name = roomDisplayName(*r);
                if (r->value("is_direct", false))
                    name = (st.showEmoji ? "\xf0\x9f\x92\xac " : "[DM] ") + name;
                std::string row = mark + name + " ("
                                + std::to_string(roomMessageCount(st.db, rid)) + ")";
                // The last-message time, right-aligned (Element Classic).
                std::string ltime = roomLastTime(st.db, rid, st.showSeconds);
                if (!ltime.empty()) {
                    int tl = displayWidth(ltime);
                    int baseW = W - tl - 1;
                    row = clip(row, baseW);
                    row += std::string(std::max(1, W - baseW - tl), ' ')
                         + "\x1b[90m" + ltime + "\x1b[0m";
                }
                int thr = roomThreadCount(st.db, rid);
                if (thr > 0) {
                    row += (st.showEmoji ? " \xf0\x9f\xa7\xb5" : " (threads ")
                         + std::to_string(thr) + (st.showEmoji ? "" : ")");
                }
                stream.push_back(row);
                // Element Classic: the room name on its own row, the last
                // message preview (dim grey, indented) right below it.
                std::string last = roomLastMsg(st.db, rid);
                if (!last.empty()) {
                    stream.push_back("[90m  " + clip(last, W - 2) + "[0m");
                }
            }
            section = " Rooms ";
        } else if (st.mobileTab == 1) {
            stream.push_back(clip("── " + roomName + " ──", W));
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
            out += (src < total) ? clip(stream[static_cast<size_t>(src)], W) + "\n"
                                 : "\n";
        }
        out += repeat('=', W) + "\n";
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
               " find <q> | search <q> | dump <room> | verify | help | quit\n";
        return out;
    }

    // Clamp the scroll into [0, contentRows - rows] (state stays const).
    int visCount = st.roomFilter.empty() ? contentRows(st)
                                         : static_cast<int>(visible.size());
    visCount = std::max(visCount, static_cast<int>(centerRows.size()));
    visCount = std::max(visCount, static_cast<int>(rightRows.size()));
    int maxScroll = std::max(0, visCount - rows);
    int scroll = std::min(std::max(0, st.scroll), maxScroll);
    if (scroll > 0) out += "  ^ more above (scroll up)\n";
    if (scroll + rows < contentRows(st)) out += "  v more below (scroll down)\n";
    for (int i = 0; i < rows; ++i) {
        int src = scroll + i;  // the content row this view row shows
        std::string left, center, right;
        if (src < static_cast<int>(visible.size())) {
            const auto& r = *visible[static_cast<size_t>(src)];
            std::string rid = r.value("room_id", "");
            std::string mark = rid == st.currentRoomId ? "*" : " ";
            std::string name = roomDisplayName(r);
            if (r.value("is_direct", false))
                name = (st.showEmoji ? "💬 " : "[DM] ") + name;
            left = mark + name + " (" + std::to_string(roomMessageCount(st.db, rid)) + ")";
            int thr = roomThreadCount(st.db, rid);
            if (thr > 0) {
                left += (st.showEmoji ? " 🧵" : " (threads ") + std::to_string(thr)
                      + (st.showEmoji ? "" : ")");
            }
            // The last message preview, like Element's room list.
            std::string last = roomLastMsg(st.db, rid);
            if (!last.empty()) {
                int avail = leftW - displayWidth(left) - 1;
                if (avail >= 6) {
                    left += "[90m" + clip(" · " + last, avail) + "[0m";
                }
            }
        }
        if (src < static_cast<int>(centerRows.size())) {
            center = centerRows[static_cast<size_t>(src)];
        }
        if (src < static_cast<int>(rightRows.size())) {
            right = rightRows[static_cast<size_t>(src)];
        }
        out += pad(left, static_cast<size_t>(leftW)) + PIPE + "|" + X
             + pad(center, static_cast<size_t>(centerW)) + PIPE + "|" + X
             + pad(right, static_cast<size_t>(rightW)) + "\n";
    }

    // Status line
    out += repeat('=', W) + "\n";
    std::string pos;
    if (contentRows(st) > rows) {
        pos = " [rows " + std::to_string(scroll + 1) + "-"
            + std::to_string(scroll + rows) + " of "
            + std::to_string(contentRows(st)) + "]";
    }
    out += "proxy: " + st.proxyLabel + " | scroll: up/down/top/bottom" + pos;
    if (!st.statusNote.empty()) {
        // The last action's summary lives in the RIGHT-BOTTOM corner.
        out += " | " + st.statusNote;
    }
    out += "\n";
    out += "open <room> | send <room> <text> | find <q> | search <q> | thread |"
           " dump <room> | verify | presence | ban/kick | media | space | help | quit\n";
    return out;
}

} // namespace

// The about screen: the title, the (c) line, the user's aewan logo
// (the rising line with the /\ dip), the tagline and the info.
void printAbout(const std::string& proxyLabel, const std::string& accountLabel) {
    const char* X = "\x1b[0m";
    std::cout << "\x1b[1;31mprogressive-cli\x1b[0m"
              << " — terminal Matrix client\n";
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

// One-shot 'matrixcli about' — the about screen without the ui.
int cmdAbout(const cli::Args&) {
    // No session/network init — the about screen must be clean and instant.
    printAbout("", "");
    return 0;
}

int cmdAsciiUi(const cli::Args& args) {
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cerr << "Cannot open matrixcli.db — run 'matrixcli demo populate' first"
                  << std::endl;
        return 1;
    }

    UiState st;
    st.db = &dbi;
    st.rooms = dbi.listRooms();
    if (st.rooms.empty()) {
        std::cerr << "No rooms cached. Run 'matrixcli demo populate' or sync first."
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
    // Demo/offline: static presence so the right panel shows the letters.
    // The demo events carry short senders ("@alice") — key both forms.
    if (st.accountLabel == "demo (offline)") {
        struct { const char* full; const char* short_; const char* letter; } demoPres[] = {
            {"@alice:demo.local", "@alice", "O"},
            {"@bob:demo.local", "@bob", "A"},
            {"@charlie:demo.local", "@charlie", "F"},
            {"@you:demo.local", "@you", "O"},
        };
        for (auto& p : demoPres) {
            st.presence[p.full] = p.letter;
            st.presence[p.short_] = p.letter;
        }
    }
    std::string initial = args.positional.empty() ? "" : args.positional[0];
    loadRoomIntoState(st, initial);
    if (st.currentRoomId.empty() && !st.rooms.empty()) {
        loadRoomIntoState(st, st.rooms.front().value("room_id", ""));
    }


    // --static thread <room> / --static threads: the right panel becomes
    // the thread list instead of a room being opened.
    if (initial == "thread" && args.positional.size() >= 2) {
        loadRoomIntoState(st, args.positional[1]);
        st.rightPanel = 1;
        st.threadRoomId = st.currentRoomId;
    } else if (initial == "threads") {
        st.rightPanel = 3;
        if (args.positional.size() >= 2) loadRoomIntoState(st, args.positional[1]);
    }
    // Non-interactive flags (also usable in the REPL): --ids, --time-full,
    // --right members|threads, --limit N.
    if (args.options.count("ids")) st.showIds = true;
    if (args.options.count("time-full") || args.options.count("sec")) st.showSeconds = true;
    if (args.options.count("no-emoji")) st.showEmoji = false;
    if (args.options.count("limit")) {
        try { st.limit = std::stoi(args.options.at("limit")); } catch (...) {}
        loadRoomIntoState(st, st.currentRoomId);
    }
    if (args.options.count("right")) {
        std::string r = args.options.at("right");
        if (r == "threads") {
            st.rightPanel = 3;
        } else if (r == "list") {
            st.rightPanel = 1;
            st.threadRoomId = st.currentRoomId;
        } else if (r == "thread" && args.options.count("thread-root")) {
            st.rightPanel = 2;
            st.threadRootId = args.options.at("thread-root");
            st.threadRoomId = st.currentRoomId;
            auto events = dbi.getEvents(st.currentRoomId, 300);
            st.threadReplies.clear();
            for (const auto& ev : events) {
                if (eventThreadRoot(ev) == st.threadRootId) st.threadReplies.push_back(ev);
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
    if (args.options.count("space")) st.activeSpace = args.options.at("space");
    // Element Classic: with a room on the command line, open it in the
    // Chat tab right away; without one, land on the Rooms tab.
    if (st.mobile && !initial.empty()) st.mobileTab = 1;
    if (args.options.count("rows")) {
        try { st.limitRows = std::stoi(args.options.at("rows")); } catch (...) {}
    }
    if (args.options.count("scroll")) {
        try { st.scroll = std::stoi(args.options.at("scroll")); } catch (...) {}
    }
    std::cout << drawFrame(st) << std::flush;

    // Pure CLI / non-interactive mode: draw the frame once and exit
    // (pipe-friendly: matrixcli ui --static [room] | less).
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
                std::cout << "── " << eventBody(ev) << " (demo preview) ──" << std::endl;
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
            std::cout << "  open <room>       switch the center panel to a room\n"
                         "  view [room]       show a room (same as open)\n"
                         "  send <room> <text>  append a message (offline demo)\n"
                         "  thread <room> [root]  list threads / show one thread\n"
                         "  up / down [n]     scroll the viewport (default 5 rows)\n"
                         "  top / bottom      jump to the top / bottom\n"
                         "  scroll <n>        scroll down by n rows\n"
                         "  rooms             reload the room list from the cache\n"
                         "  refresh           redraw the frame\n"
                         "  quit / exit       leave the ui mode\n";
            continue;
        }
        if (a.command == "open" || a.command == "view") {
            std::string q = a.positional.empty() ? st.currentRoomId : a.positional[0];
            loadRoomIntoState(st, q);
            if (st.mobile) {
                st.mobileTab = 1;  // Element Classic: opening jumps to Chat
                st.scroll = 0;
            }
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "thread") {
            if (!a.positional.empty() && a.positional[0] == "off") {
                st.rightPanel = 0;
                st.statusNote = "right panel: members";
                std::cout << drawFrame(st) << std::flush;
                continue;
            }
            if (a.positional.size() < 2) {
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
                st.statusNote = "right panel: threads of " + tRoom;
                std::cout << drawFrame(st) << std::flush;
                continue;
            }
            // One thread in the right panel: root + replies.
            std::string q = a.positional[0];
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
            std::string root = a.positional[1];
            auto events = st.db->getEvents(tRoom, 300);
            st.threadRootId = root;
            st.threadReplies.clear();
            for (const auto& ev : events) {
                if (eventThreadRoot(ev) == root) st.threadReplies.push_back(ev);
            }
            st.rightPanel = 2;
            st.threadRoomId = tRoom;
            st.statusNote = "right panel: thread " + root.substr(0, 12)
                          + " (" + std::to_string(st.threadReplies.size()) + " replies)";
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- threads: threads across ALL rooms (the Element thread list) ----
        if (a.command == "threads") {
            st.rightPanel = 3;
            st.statusNote = "right panel: threads in all rooms";
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "rooms") {
            st.rooms = dbi.listRooms();
            loadRoomIntoState(st, st.currentRoomId);
            std::cout << drawFrame(st) << std::flush;
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
            else if (a.command == "bottom") st.scroll = contentRows(st);
            else st.scroll += step;  // "scroll <n>" = down by n
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "spaces") {
            st.mobileTab = 0;
            st.scroll = 0;
            std::string list = "spaces: all";
            for (const auto& r : st.rooms) {
                if (r.value("is_space", false)) list += ", " + r.value("name", "?");
            }
            st.statusNote = list;
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "space") {
            std::string q = a.positional.empty() ? "all" : a.positional[0];
            st.activeSpace.clear();
            if (q != "all" && q != "-") {
                // Only spaces are matchable (so "tech" never hits "#techno").
                for (const auto& r : st.rooms) {
                    if (!r.value("is_space", false)) continue;
                    std::string id = r.value("room_id", "");
                    std::string name = r.value("name", "");
                    if (id == q || name == q) {
                        st.activeSpace = id;
                        break;
                    }
                }
                if (st.activeSpace.empty()) {
                    std::string ql = q;
                    for (auto& ch : ql) ch = static_cast<char>(std::tolower(ch));
                    for (const auto& r : st.rooms) {
                        if (!r.value("is_space", false)) continue;
                        std::string name = r.value("name", "");
                        std::string nl = name;
                        for (auto& ch : nl) ch = static_cast<char>(std::tolower(ch));
                        if (nl.find(ql) != std::string::npos) {
                            st.activeSpace = r.value("room_id", "");
                            break;
                        }
                    }
                }
                if (st.activeSpace.empty()) {
                    st.statusNote = "no such space: " + q;
                }
            }
            st.scroll = 0;
            if (st.mobile) st.mobileTab = 0;
            st.rooms = dbi.listRooms();
            loadRoomIntoState(st, st.currentRoomId);
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "rooms" && st.mobile) {
            st.mobileTab = 0;
            st.scroll = 0;
            st.rooms = dbi.listRooms();
            loadRoomIntoState(st, st.currentRoomId);
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "chat" && st.mobile) {
            st.mobileTab = 1;
            st.scroll = 0;
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "people" && st.mobile) {
            st.mobileTab = 2;
            st.scroll = 0;
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "mobile") {
            std::string v = a.positional.empty() ? "on" : a.positional[0];
            st.mobile = (v == "on" || v == "1" || v == "true" || v == "yes");
            st.scroll = 0;
            st.statusNote = st.mobile
                ? "smartphone layout: stacked sections"
                : "desktop layout: three columns";
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        if (a.command == "refresh") {
            std::cout << drawFrame(st) << std::flush;
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
            loadRoomIntoState(st, st.currentRoomId);
            std::cout << drawFrame(st) << std::flush;
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
                loadRoomIntoState(st, roomId);
            }
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- find: filter the room list ----
        if (a.command == "find" || a.command == "filter") {
            if (a.positional.empty()) {
                st.roomFilter.clear();
                st.statusNote = "room filter cleared";
            } else {
                st.roomFilter = a.positional[0];
                st.statusNote = "rooms matching '" + st.roomFilter + "'";
            }
            st.scroll = 0;
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
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
                    loadRoomIntoState(st, "");
                    if (st.currentRoomId.empty() && !st.rooms.empty()) {
                        loadRoomIntoState(st, st.rooms.front().value("room_id", ""));
                    }
                    st.presence.clear();
                    st.statusNote = "account switched to " + uid;
                    found = true;
                    break;
                }
            }
            if (!found) std::cout << "No such account: " << a.positional[0] << std::endl;
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- moderation + room settings (delegate to the registry) ----
        if (a.command == "ban" || a.command == "kick" || a.command == "unban" ||
            a.command == "topic" || a.command == "roomname") {
            auto cliHandler = CommandRegistry::instance().findCli(a.command);
            if (!cliHandler) {
                std::cout << a.command << " not available in this build." << std::endl;
                continue;
            }
            cliHandler(a);
            std::cout << drawFrame(st) << std::flush;
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
                int cols = std::max(30, terminalWidth() - 4);
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
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- dump: export the room like Element Web ----
        if (a.command == "dump" || a.command == "export") {
            if (a.positional.empty()) {
                std::cout << "Usage: dump <room> [room2 ...] [--format json|txt|html]"
                             " [--out dir] [--media] [--server]"
                          << std::endl;
                std::cout << "  --media          download media files (default: no media)\n"
                             "  --limit N        export only the last N events\n"
                             "  --order asc|desc chronological (default asc, oldest first)\n"
                             "  --types list     segment by type: messages|media|system"
                             " (comma list, default all)\n"
                             "  --media-max MB   skip media files larger than MB megabytes\n"
                             "  --archive zip|tar.gz  pack the export into an archive"
                             " (zip needs the 'zip' tool)\n"
                             "  --server        dump the FULL history from the server (paginated),"
                             " not just the cache"
                          << std::endl;
                continue;
            }
            std::string fmt = a.options.count("format") ? a.options.at("format") : "json";
            std::string outDir = a.options.count("out") ? a.options.at("out") : ".";
            bool withMedia = a.options.count("media");
            int limit = 0;
            if (a.options.count("limit")) {
                try { limit = std::stoi(a.options.at("limit")); } catch (...) {}
            }
            bool descOrder = a.options.count("order") && a.options.at("order") == "desc";
            std::string types = a.options.count("types") ? a.options.at("types") : "all";
            long long mediaMaxBytes = -1;
            if (a.options.count("media-max")) {
                try {
                    double mb = std::stod(a.options.at("media-max"));
                    mediaMaxBytes = static_cast<long long>(mb * 1024 * 1024);
                } catch (...) {}
            }
            std::string archive = a.options.count("archive") ? a.options.at("archive") : "";
            // Media needs a session (downloads); --media without one = note.
            bool mediaSession = withMedia && pcore::init() && pcore::loadSavedSession();
            if (withMedia && !mediaSession) {
                std::cout << "note: --media needs a logged-in session — exporting events "
                             "only (mxc URLs are in the JSON)." << std::endl;
            }
            // Type segment filter.
            auto typeAllowed = [&types](const matrix::Event& ev) {
                if (types == "all") return true;
                bool isMedia = false;
                bool isSystem = ev.type == "m.room.member" || ev.type == "m.room.name" ||
                                ev.type == "m.room.topic" || ev.type == "m.room.avatar" ||
                                ev.type == "m.room.create" || ev.type == "m.room.power_levels" ||
                                ev.type == "m.room.join_rules";
                if (ev.type == "m.room.message" && ev.content.is_object()) {
                    auto mt = ev.content.find("msgtype");
                    if (mt != ev.content.end() && mt->is_string()) {
                        std::string m = mt->get<std::string>();
                        isMedia = m == "m.image" || m == "m.video" || m == "m.audio" ||
                                  m == "m.file" || m == "m.sticker";
                    }
                }
                if (ev.type == "m.sticker") isMedia = true;
                bool isMsg = ev.type == "m.room.message" || ev.type == "m.sticker";
                if (types.find("messages") != std::string::npos && isMsg && !isMedia) return true;
                if (types.find("media") != std::string::npos && isMedia) return true;
                if (types.find("system") != std::string::npos && isSystem) return true;
                return false;
            };
            bool wantServer = a.options.count("server");
            bool serverSession = wantServer && pcore::init() && pcore::loadSavedSession();
            if (wantServer && !serverSession) {
                std::cout << "note: --server needs a logged-in session — using the cache "
                             "instead." << std::endl;
            }
            for (const auto& roomQ : a.positional) {
                std::string rid = roomQ;
                std::string rname = roomQ;
                for (const auto& r : st.rooms) {
                    std::string id = r.value("room_id", "");
                    std::string name = r.value("name", "");
                    if (id == roomQ || id.find(roomQ) != std::string::npos ||
                        name == roomQ || name.find(roomQ) == 0 ||
                        name.find(roomQ) != std::string::npos) {
                        rid = id;
                        rname = name.empty() ? rid : name;
                        break;
                    }
                }
                std::vector<matrix::Event> events;
                if (serverSession) {
                    // Full history from the server: walk /messages?dir=b from
                    // the top (newest) backwards until the beginning.
                    auto& core = pcore::core();
                    auto client = core.client;
                    std::string from = "";
                    int received = 0;
                    std::cout << "dump: " << rname << " — fetching full history from the "
                                 "server..." << std::endl;
                    for (int guard = 0; guard < 2000; ++guard) {
                        auto r = client->getMessages(rid, from, 100);
                        if (!r.ok) {
                            std::cout << "  fetch error: " << r.error.message << std::endl;
                            break;
                        }
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
                                events.push_back(std::move(e));
                            }
                            received += static_cast<int>(chunk.size());
                            end = j.value("end", "");
                            std::cout << "  received " << received << " events..." << std::endl;
                            if (chunk.empty() || end.empty() || end == from) break;
                        } catch (...) {
                            break;
                        }
                        from = end;
                    }
                } else {
                    events = dbi.getEvents(rid, 50000);
                    std::cout << "dump: " << rname << " — received " << events.size()
                              << " events, processing..." << std::endl;
                }
                // Type segment filter.
                if (types != "all") {
                    std::vector<matrix::Event> filtered;
                    for (const auto& ev : events) {
                        if (typeAllowed(ev)) filtered.push_back(ev);
                    }
                    events = std::move(filtered);
                }
                // --limit N: keep only the last N (the newest) events.
                if (limit > 0 && static_cast<int>(events.size()) > limit) {
                    events.erase(events.begin(), events.end() - limit);
                }
                // --order desc: newest first (asc = chronological, the default).
                if (descOrder) {
                    std::reverse(events.begin(), events.end());
                }
                std::string safeName = rname;
                for (auto& c : safeName) {
                    if (c == '/' || c == '#' || c == '!' || c == ':' || c == ' ') c = '_';
                }
                std::string path = outDir + "/" + safeName + "." + fmt;
                std::ofstream fout(path);
                if (!fout) {
                    std::cout << "  cannot write " << path << std::endl;
                    continue;
                }
                int processed = 0;
                if (fmt == "json") {
                    nlohmann::json j;
                    j["room_id"] = rid;
                    j["name"] = rname;
                    j["events"] = nlohmann::json::array();
                    for (const auto& ev : events) {
                        nlohmann::json e;
                        e["event_id"] = ev.event_id;
                        e["sender"] = ev.sender;
                        e["type"] = ev.type;
                        e["origin_server_ts"] = ev.origin_server_ts;
                        e["content"] = ev.content;
                        j["events"].push_back(e);
                        processed++;
                    }
                    fout << j.dump(1) << std::endl;
                } else if (fmt == "txt") {
                    for (const auto& ev : events) {
                        std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000);
                        std::tm tm{};
                        localtime_r(&t, &tm);
                        char buf[20];
                        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                                      tm.tm_hour, tm.tm_min, tm.tm_sec);
                        fout << "[" << buf << "] " << senderShort(ev.sender) << ": "
                             << eventBody(ev) << std::endl;
                        processed++;
                    }
                } else if (fmt == "html") {
                    fout << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                         << "<title>" << rname << "</title></head><body><h1>"
                         << rname << "</h1>" << std::endl;
                    for (const auto& ev : events) {
                        fout << "<p><b>" << senderShort(ev.sender) << "</b> "
                             << eventBody(ev) << "</p>" << std::endl;
                        processed++;
                    }
                    fout << "</body></html>" << std::endl;
                } else {
                    std::cout << "  unknown format '" << fmt
                              << "' (json|txt|html)" << std::endl;
                    continue;
                }
                fout.close();
                // Media pass: download the attached files into <out>/<name>_media/.
                int mediaSaved = 0;
                if (withMedia && mediaSession) {
                    std::string mediaDir = outDir + "/" + safeName + "_media";
                    std::filesystem::create_directories(mediaDir);
                    auto& core = pcore::core();
                    auto client = core.client;
                    for (const auto& ev : events) {
                        if (!ev.content.is_object()) continue;
                        std::string mxc, key, iv, sha, mime;
                        auto urlIt = ev.content.find("url");
                        if (urlIt != ev.content.end() && urlIt->is_string()) {
                            mxc = urlIt->get<std::string>();
                        }
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
                        if (mxc.empty()) continue;
                        auto infoIt = ev.content.find("info");
                        if (mediaMaxBytes > 0 && infoIt != ev.content.end() &&
                            infoIt->is_object()) {
                            auto sz = infoIt->find("size");
                            if (sz != infoIt->end() && sz->is_number()) {
                                if (sz->get<long long>() > mediaMaxBytes) {
                                    std::cout << "  media skip (over --media-max): "
                                              << ev.event_id << std::endl;
                                    continue;
                                }
                            }
                        }
                        if (infoIt != ev.content.end() && infoIt->is_object()) {
                            auto m = infoIt->find("mimetype");
                            if (m != infoIt->end() && m->is_string()) mime = m->get<std::string>();
                        }
                        std::string ext = "bin";
                        if (mime == "image/png") ext = "png";
                        else if (mime == "image/jpeg") ext = "jpg";
                        else if (mime == "image/gif") ext = "gif";
                        else if (mime == "image/webp") ext = "webp";
                        else if (mime == "video/mp4") ext = "mp4";
                        else if (mime == "audio/mpeg") ext = "mp3";
                        else if (mime == "audio/ogg") ext = "ogg";
                        std::string local = mediaDir + "/" + ev.event_id.substr(0, 20) + "." + ext;
                        std::vector<uint8_t> bytes;
                        if (!key.empty()) {
                            auto r = client->downloadMediaEncrypted(mxc, key, iv, sha);
                            if (r.ok) bytes = r.data;
                        } else {
                            auto r = client->downloadMedia(mxc, 0, 0);
                            if (r.ok) bytes = r.data;
                        }
                        if (bytes.empty()) {
                            std::cout << "  media skip (download failed): " << ev.event_id
                                      << std::endl;
                            continue;
                        }
                        std::ofstream mout(local, std::ios::binary);
                        if (!mout) continue;
                        mout.write(reinterpret_cast<const char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
                        mout.close();
                        mediaSaved++;
                    }
                }
                // --archive zip|tar.gz: pack the dump + the media folder.
                if (!archive.empty()) {
                    std::string archPath = outDir + "/" + safeName + "." + archive;
                    if (archive == "zip") {
                        std::string cmd = "cd '" + outDir + "' && zip -r -q '" + safeName
                                        + ".zip' '" + safeName + "." + fmt + "'"
                                        + (withMedia ? " '" + safeName + "_media'" : "")
                                        + " 2>/dev/null";
                        if (std::system(cmd.c_str()) == 0) {
                            std::cout << "  archive: " << archPath << std::endl;
                        } else {
                            std::cout << "  archive failed (is 'zip' installed?)" << std::endl;
                        }
                    } else if (archive == "tar.gz") {
                        std::string cmd = "cd '" + outDir + "' && tar -czf '" + safeName
                                        + ".tar.gz' '" + safeName + "." + fmt + "'"
                                        + (withMedia ? " '" + safeName + "_media'" : "")
                                        + " 2>/dev/null";
                        if (std::system(cmd.c_str()) == 0) {
                            std::cout << "  archive: " << archPath << std::endl;
                        } else {
                            std::cout << "  archive failed" << std::endl;
                        }
                    } else {
                        std::cout << "  unknown --archive '" << archive
                                  << "' (zip|tar.gz)" << std::endl;
                    }
                }
                std::cout << "  done: " << path << " (" << processed << " events"
                          << (withMedia ? ", " + std::to_string(mediaSaved) + " media files)" : ")")
                          << std::endl;
                st.statusNote = "dump: " + rname + " — " + std::to_string(events.size())
                              + " events, " + std::to_string(processed) + " processed -> " + path
                              + (withMedia ? " + " + std::to_string(mediaSaved) + " media" : "");
            }
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
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
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- emoji on|off: emoji glyphs or ASCII fallbacks ----
        if (a.command == "emoji") {
            if (a.positional.empty() || a.positional[0] == "on") st.showEmoji = true;
            else st.showEmoji = false;
            st.statusNote = std::string("emoji ") + (st.showEmoji ? "on" : "off (ASCII)");
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- images on|off: full image cards ----
        if (a.command == "images") {
            if (a.positional.empty() || a.positional[0] == "on") st.showImages = true;
            else st.showImages = false;
            st.statusNote = std::string("images ") + (st.showImages ? "full cards" : "compact");
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- reply: send a reply to a message ----
        if (a.command == "reply") {
            if (a.positional.size() < 3) {
                std::cout << "Usage: reply <room> <event_id> <text>" << std::endl;
                continue;
            }
            auto cliHandler = CommandRegistry::instance().findCli("reply");
            if (!cliHandler) {
                std::cout << "reply not available in this build." << std::endl;
                continue;
            }
            cliHandler(a);
            std::cout << drawFrame(st) << std::flush;
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
            int64_t dayMs = parseDayMs(a.positional[1]);
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
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- pin / unpin ----
        if (a.command == "pin" || a.command == "unpin") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: " << a.command << " <room> <event_id>" << std::endl;
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
            std::cout << drawFrame(st) << std::flush;
            continue;
        }
        // ---- search filters: --sender, --since, --until ----
        if (a.command == "search") {
            if (a.positional.empty()) {
                std::cout << "Usage: search <query> [--limit N] [--sender @u]"
                             " [--since YYYY-MM-DD] [--until YYYY-MM-DD]" << std::endl;
                continue;
            }
            int limit = 20;
            if (a.options.count("limit")) {
                try { limit = std::stoi(a.options.at("limit")); } catch (...) {}
            }
            int64_t sinceMs = a.options.count("since") ? parseDayMs(a.options.at("since")) : -1;
            int64_t untilMs = a.options.count("until") ? parseDayMs(a.options.at("until")) : -1;
            std::string senderF = a.options.count("sender") ? a.options.at("sender") : "";
            auto hits = dbi.search(a.positional[0], limit * 4);
            int shown = 0;
            for (const auto& h : hits) {
                if (shown >= limit) break;
                if (!senderF.empty() && h.value("sender", "") != senderF) continue;
                int64_t ts = h.value("ts", h.value("origin_server_ts", 0LL));
                if (sinceMs > 0 && ts < sinceMs) continue;
                if (untilMs > 0 && ts > untilMs) continue;
                std::string rid = h.value("room_id", "");
                std::string sender = h.value("sender", "");
                std::string body = h.value("body", "");
                std::cout << "  [" << senderShort(sender) << "] "
                          << clip(rid, 24) << ": " << clip(body, 60) << std::endl;
                shown++;
            }
            if (shown == 0) {
                std::cout << "No matches for '" << a.positional[0] << "'"
                          << (senderF.empty() ? "" : " from " + senderF) << std::endl;
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
            std::cout << drawFrame(st) << std::flush;
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
            std::cout << drawFrame(st) << std::flush;
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
            if (!st.messages.empty()) last = eventBody(st.messages.back());
            std::string cmd = "notify-send 'matrixcli: " + rname + "' '" + clip(last, 80)
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

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
                if (eventBody(prev).empty()) break;
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

// The m.room.tombstone successor of the room ("" when none) — the receive
// side of a room upgrade.

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

std::string matchRoomInCache(const std::vector<nlohmann::json>& rooms,
                             const std::string& query) {
    std::string q = query;
    if (q.size() > 1 && q[0] == '#') {
        auto colon = q.find(':');
        if (colon != std::string::npos) q = q.substr(0, colon);
    }
    if (q.empty()) return "";
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    std::string lq = lower(q);
    std::string best;
    int bestScore = 0;
    for (const auto& r : rooms) {
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        std::string alias = r.value("canonical_alias", "");
        std::string a = alias.size() > 1 && alias[0] == '#'
                            ? alias.substr(1) : alias;
        std::string lid = lower(id), la = lower(a), ln = lower(name);
        if (lid == lq || la == lq || ln == lq) return id;
        auto score = [&](int s) -> bool {
            if (s > bestScore) { bestScore = s; best = id; return true; }
            return false;
        };
        if (lid.find(lq) == 0 || la.find(lq) == 0 || ln.find(lq) == 0)
            score(100 - static_cast<int>(name.size()));
        if (ln.find(lq) != std::string::npos || la.find(lq) != std::string::npos)
            score(50);
    }
    return best;
}

void loadRoomIntoStateImpl(UiState& st, const std::string& query) {
    st.currentRoomId.clear();
    st.currentRoomId = matchRoomInCache(st.rooms, query);
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
        if (ev.type == "m.room.power_levels" && ev.content.is_object()) {
            // The name-dedup skip below would swallow this: the pl event's
            // sender is usually the first member seen (newest first), so
            // the levels are read here, before any skip.
            st.powerLevelsEvent = ev.content;
            auto users = ev.content.find("users");
            if (users != ev.content.end() && users->is_object()) {
                for (auto& [uid, lvl] : users->items()) {
                    if (lvl.is_number()) st.powerLevels[uid] = lvl.get<int>();
                }
            }
        }
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
    // Power levels first: admins, then moderators, then the rest (stable).
    std::stable_sort(st.members.begin(), st.members.end(),
        [&](const std::string& a, const std::string& b) {
            return st.powerLevels[a] > st.powerLevels[b];
        });
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
    // Notifications for the bottom-right corner: pings (@me mentions),
    // replies to my messages, read receipts in rooms monitored at 100%
    // ("seen your message" when a receipt targets my own event, "read a
    // message" otherwise). Newest first, capped at 12.
    st.notifications.clear();
    {
        const std::string myId = st.accountLabel == "demo (offline)"
                                     ? "@you" : "@" + st.accountLabel;
        size_t sep = myId.find('@', 1);
        const std::string localpart = "@" + myId.substr(1,
            sep == std::string::npos ? std::string::npos : sep - 1);
        std::vector<Notification> found;
        auto mkPreview = [](std::string s) {
            for (auto& ch : s) {
                if (ch == '\n' || ch == '\r') ch = ' ';
            }
            if (s.size() > 40) s = s.substr(0, 40) + "\xe2\x80\xa6";
            return s;
        };
        auto collect = [&](const matrix::Event& ev,
                           const std::string& rname,
                           const std::unordered_set<std::string>& myEvents) {
            if (found.size() >= 48) return;
            if (ev.type == "m.receipt" && ev.content.is_object()) {
                if (st.db->getSetting("monitor:" + ev.room_id, "0") != "100")
                    return;
                for (const auto& [eid, readers] : ev.content.items()) {
                    if (!readers.is_object()) continue;
                    auto rit = readers.find("m.read");
                    if (rit == readers.end() || !rit->is_object()) continue;
                    for (const auto& [uid, info] : rit->items()) {
                        if (uid == myId || !info.is_object()) continue;
                        int64_t rts = 0;
                        auto tit = info.find("ts");
                        if (tit != info.end() && tit->is_number())
                            rts = tit->get<int64_t>();
                        if (rts <= 0) rts = ev.origin_server_ts;
                        bool mine = myEvents.count(eid) != 0;
                        found.push_back({rts, rname,
                            senderShortImpl(uid)
                                + (mine ? " saw your message" : " read a message"),
                            mine ? 3 : 0});
                    }
                }
                return;
            }
            if (ev.type == "m.room.message" && ev.content.is_object() &&
                ev.sender != myId && ev.content.contains("body")) {
                std::string body = ev.content["body"].get<std::string>();
                bool ping = body.find(myId) != std::string::npos ||
                            body.find(localpart) != std::string::npos;
                // A reply to MY message ("m.in_reply_to"/"m.thread" target).
                bool reply = false;
                if (ev.content.contains("m.relates_to") &&
                    ev.content["m.relates_to"].is_object()) {
                    auto rt = ev.content["m.relates_to"];
                    std::string target;
                    if (rt.contains("m.in_reply_to") &&
                        rt["m.in_reply_to"].is_object() &&
                        rt["m.in_reply_to"].contains("event_id")) {
                        target = rt["m.in_reply_to"]["event_id"]
                                     .get<std::string>();
                    } else if (rt.value("rel_type", "") == "m.thread" &&
                               rt.contains("event_id")) {
                        target = rt["event_id"].get<std::string>();
                    }
                    if (!target.empty() && myEvents.count(target))
                        reply = true;
                }
                if (reply) {
                    found.push_back({ev.origin_server_ts, rname,
                        senderShortImpl(ev.sender) + " replied to you: "
                            + mkPreview(body), 2});
                } else if (ping) {
                    found.push_back({ev.origin_server_ts, rname,
                        senderShortImpl(ev.sender) + " pinged you: "
                            + mkPreview(body), 1});
                }
            }
        };
        for (const auto& r : st.rooms) {
            std::string rname = r.value("name", r.value("room_id", "?"));
            auto evs = st.db->getEvents(r.value("room_id", ""), 300);
            std::unordered_set<std::string> myEvents;
            for (const auto& ev : evs) {
                if (ev.sender == myId) myEvents.insert(ev.event_id);
            }
            for (const auto& ev : evs) collect(ev, rname, myEvents);
            if (found.size() >= 48) break;
        }
        std::stable_sort(found.begin(), found.end(),
            [](const Notification& a, const Notification& b) {
                return a.ts > b.ts;
            });
        if (found.size() > 20) found.resize(20);
        st.notifications = std::move(found);
    }
}

// The thread rows of a room ("⤷ preview (N)") for the right panel.
// Resolve a thread root: "3" = the N-th thread of the room (the list
// order), otherwise the root id or a substring of it. Empty = not found.
// The room's join rule ("public", "invite", "restricted", "knock") from
// the first m.room.join_rules event — the invite rows show whether the
// room is open or closed.



// Resolve a space query (id, #alias[:server] or name) to the space id.


// The via arguments for a permalink: the distinct server names of the
// room's members. limit = 0 means ALL of them (the user's choice), else
// the first N — Element sends 3 by default.

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
            if (t[i] == '*') {
                // The *italic* pair; the **bold** branch above already won.
                auto end = t.find('*', i + 1);
                if (end != std::string::npos) {
                    r += ANSI_ITALIC + t.substr(i + 1, end - i - 1)
                       + ANSI_RESET;
                    i = end + 1;
                    continue;
                }
            }
            if (t[i] == '[') {
                // The [text](url) link: an OSC 8 hyperlink, blue underlined
                // text + the dim URL (renders as plain text elsewhere).
                auto close = t.find("](", i + 1);
                if (close != std::string::npos && close + 1 < t.size() &&
                    t[close + 1] == '(') {
                    auto end = t.find(')', close + 2);
                    if (end != std::string::npos) {
                        std::string url =
                            t.substr(close + 2, end - close - 2);
                        r += "\x1b]8;;" + url + "\x1b\\\x1b[4;34m"
                           + t.substr(i + 1, close - i - 1)
                           + "\x1b[0m\x1b]8;;\x1b\\"
                           + "\x1b[90m(" + url + ")\x1b[0m";
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

// A colour name → the ANSI SGR code ("" = default/none). Names cover the
// basic palette + grey + the bright variants.
std::string ansiColourCode(const std::string& name) {
    static const std::map<std::string, std::string> cols = {
        {"black", "30"},   {"red", "31"},       {"green", "32"},
        {"yellow", "33"},  {"blue", "34"},      {"magenta", "35"},
        {"cyan", "36"},    {"white", "37"},     {"grey", "90"},
        {"gray", "90"},    {"brightred", "91"}, {"brightgreen", "92"},
        {"brightyellow", "93"}, {"brightblue", "94"}, {"brightmagenta", "95"},
        {"brightcyan", "96"},   {"brightwhite", "97"},
    };
    auto it = cols.find(name);
    return it != cols.end() ? it->second : "";
}

// A colour spec for --names/--mxids: either one colour for every panel
// ("grey") or per-panel pairs ("left:grey,center:blue,middle:red").
// dst[0]=left panel, dst[1]=center, dst[2]=right ("" = keep the default).
void applyColourSpec(const std::string& spec, std::string (&dst)[3]) {
    std::string part;
    for (char c : spec + ",") {
        if (c != ',') { part += c; continue; }
        size_t colon = part.find(':');
        if (colon == std::string::npos) {
            std::string code = ansiColourCode(part);
            for (int i = 0; i < 3; ++i) dst[i] = code;
        } else {
            std::string p = part.substr(0, colon);
            int idx = 2;
            if (p == "left") idx = 0;
            else if (p == "center" || p == "middle") idx = 1;
            dst[idx] = ansiColourCode(part.substr(colon + 1));
        }
        part.clear();
    }
}

// A "panel:colour" aware SGR opener; "" means the default colour.
std::string colourOpen(const std::string& code, bool bold) {
    if (code.empty()) return bold ? "\x1b[1m" : "";
    return bold ? "\x1b[1;" + code + "m" : "\x1b[" + code + "m";
}

// The centre-panel display name, coloured with --names centre:... ("" keeps
// the default). The chatName feed: room nick > displayname > localpart.
std::string chatNameColoured(const UiState& st, const std::string& roomId,
                             const std::string& sender) {
    std::string n = chatName(st, roomId, sender);
    if (st.nameCol[1].empty()) return n;
    return colourOpen(st.nameCol[1], false) + n + "\x1b[0m";
}

std::string memberRowStr(const UiState& st, const std::string& mem,
                         bool fullIds, int panel) {
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
    // A display name that differs from the mxid string is highlighted
    // (user-selectable colour via --names, defaulting to blue).
    std::string namePart = nm;
    std::string nc = (panel >= 0 && panel < 3) ? st.nameCol[panel] : "";
    if (nc.empty()) {
        if (nm != senderShortImpl(mem)) namePart = "\x1b[34m" + nm + "\x1b[0m";
    } else {
        namePart = colourOpen(nc, false) + nm + "\x1b[0m";
    }
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
    std::string m = std::string(pc) + "[" + letter + "]" + "\x1b[0m " + namePart;
    // The mxid gets its own colour (--mxids), per panel.
    std::string mc = (panel >= 0 && panel < 3) ? st.mxidCol[panel] : "";
    if (!mc.empty()) m += " (" + colourOpen(mc, false) + mx + "\x1b[0m)";
    else m += " (" + mx + ")";
    auto pl = st.powerLevels.find(mem);
    // Every member shows the numeric level (0 included): the tiers get
    // the badge + the number with one space between them, everyone else
    // a plain dim "[n]".
    int lvl = pl != st.powerLevels.end() ? pl->second : 0;
    if (lvl < 150) {
        // Room v12+ (the m.room.create "creator"): the creator is the
        // owner (150) even when no explicit power level is set.
        for (const auto& r : st.rooms) {
            if (r.value("room_id", "") != st.currentRoomId) continue;
            if (r.value("version", 0) >= 12) {
                std::string cr = r.value("creator", "");
                std::string crLocal = cr.substr(0, cr.find(':'));
                if (crLocal == mem) lvl = 150;
            }
            break;
        }
    }
    if (lvl >= 150) m = "\xf0\x9f\x91\x91 \x1b[90m" + std::to_string(lvl) + "\x1b[0m " + m;
    else if (lvl >= 100) m = "\xf0\x9f\x9b\xa1 \x1b[90m" + std::to_string(lvl) + "\x1b[0m " + m;
    else if (lvl >= 50) m = "\xf0\x9f\x9b\xa1 \x1b[90m" + std::to_string(lvl) + "\x1b[0m " + m;
    else m = "\x1b[90m[" + std::to_string(lvl) + "]\x1b[0m " + m;
    return m;
}

} // namespace matrixcli

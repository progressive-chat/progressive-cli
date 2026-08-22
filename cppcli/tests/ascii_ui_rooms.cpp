// src/ascii_ui_rooms.cpp — the room/thread DB queries of the ASCII UI:
// roomLastMsg, tombstoneSuccessor, roomJoinRule, resolveThreadRoot,
// roomThreadList, resolveSpace, viaSuffix.
#include "ascii_ui_impl.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/string_utils.hpp"

namespace matrixcli {
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
        std::string body = eventBody(ev);
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
std::string tombstoneSuccessor(const std::vector<matrix::Event>& events) {
    for (const auto& ev : events) {
        if (ev.type != "m.room.tombstone" || !ev.content.is_object()) continue;
        auto s = ev.content.find("successor_room_id");
        if (s != ev.content.end() && s->is_string()) {
            std::string v = s->get<std::string>();
            if (!v.empty()) return v;
        }
    }
    return "";
}
std::string roomJoinRule(db::Database* db, const std::string& roomId) {
    if (!db) return "";
    auto evs = db->getEvents(roomId, 300);
    for (const auto& ev : evs) {
        if (ev.type != "m.room.join_rules" || !ev.content.is_object()) continue;
        auto r = ev.content.find("join_rule");
        if (r == ev.content.end() || !r->is_string()) return "";
        return r->get<std::string>();
    }
    return "";
}
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
            thr.push_back("⤷ " + clip(eventBody(ev), clipW) + " ("
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

int viaServerCount(db::Database* db, const std::string& roomId) {
    if (!db || roomId.empty()) return 0;
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
    return static_cast<int>(servers.size());
}
} // namespace matrixcli

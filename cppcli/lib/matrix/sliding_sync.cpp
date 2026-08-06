#include "sliding_sync.hpp"
#include "../http/http.hpp"
#include "../util/logger.hpp"

#include <stdexcept>

namespace matrixcli { namespace matrix {

// ── SlidingSyncList ──
json SlidingSyncList::toJson() const {
    json j;
    j["ranges"] = json::array({{range_start, range_end}});
    if (!sort_order.empty()) j["sort"] = json::array({sort_order});
    if (!required_state.empty()) {
        json rs = json::array();
        for (auto& entry : required_state) rs.push_back(entry);
        j["required_state"] = rs;
    }
    j["timeline_limit"] = timeline_limit;
    return j;
}

// ── SlidingSyncRequest ──
json SlidingSyncRequest::toJson() const {
    json j;
    if (!lists.empty()) {
        json lj = json::object();
        for (auto& list : lists) lj[list.name] = list.toJson();
        j["lists"] = lj;
    }
    if (!room_subscriptions.empty()) {
        json rs = json::object();
        for (auto& rid : room_subscriptions) {
            rs[rid] = {{"required_state", json::array()}, {"timeline_limit", 20}};
        }
        j["room_subscriptions"] = rs;
    }
    if (!pos.empty()) j["pos"] = pos;
    j["timeout"] = timeout_ms;

    // Extensions: E2EE + to_device + account_data
    j["extensions"] = {
        {"e2ee", {{"enabled", true}}},
        {"to_device", {{"enabled", true}}},
        {"account_data", {{"enabled", true}}}
    };
    return j;
}

// ── SlidingSyncResponse ──
bool SlidingSyncResponse::fromJson(const std::string& body) {
    try {
        auto j = json::parse(body);
        pos = j.value("pos", "");

        // Parse rooms
        if (j.contains("rooms")) {
            for (auto& [room_id, room_data] : j["rooms"].items()) {
                SlidingSyncRoom room;
                room.room_id = room_id;
                room.name = room_data.value("name", "");
                room.avatar_url = room_data.value("avatar_url", "");
                room.is_dm = room_data.value("is_dm", false);
                room.notification_count = room_data.value("notification_count", 0);
                room.highlight_count = room_data.value("highlight_count", 0);
                room.limited = room_data.value("limited", false);

                if (room_data.contains("timeline")) {
                    for (auto& ev : room_data["timeline"]) room.timeline.push_back(ev);
                }
                if (room_data.contains("required_state")) {
                    for (auto& ev : room_data["required_state"]) room.timeline.push_back(ev);
                }

                rooms.push_back(room);
            }
        }

        // Parse to_device
        if (j.contains("extensions") && j["extensions"].contains("to_device") &&
            j["extensions"]["to_device"].contains("events")) {
            for (auto& ev : j["extensions"]["to_device"]["events"]) to_device.push_back(ev);
        }

        // Parse account_data
        if (j.contains("extensions") && j["extensions"].contains("account_data") &&
            j["extensions"]["account_data"].contains("events")) {
            for (auto& ev : j["extensions"]["account_data"]["events"]) account_data.push_back(ev);
        }

        return true;
    } catch (const std::exception& e) {
        util::Logger::instance().error(std::string("Sliding sync parse failed: ") + e.what());
        return false;
    }
}

// ── SlidingSync ──
void SlidingSync::addList(const std::string& name, int range_start, int range_end) {
    SlidingSyncList list;
    list.name = name;
    list.range_start = range_start;
    list.range_end = range_end;
    list.required_state = {{"m.room.name", ""}, {"m.room.avatar", ""}, {"m.room.topic", ""},
                            {"m.room.encryption", ""}, {"m.room.join_rules", ""}};
    _lists.push_back(list);
}

void SlidingSync::subscribeRoom(const std::string& room_id) {
    if (std::find(_subscriptions.begin(), _subscriptions.end(), room_id) == _subscriptions.end())
        _subscriptions.push_back(room_id);
}

void SlidingSync::unsubscribeRoom(const std::string& room_id) {
    _subscriptions.erase(std::remove(_subscriptions.begin(), _subscriptions.end(), room_id),
                         _subscriptions.end());
}

void SlidingSync::scrollDown(int amount) {
    for (auto& list : _lists) {
        list.range_start += amount;
        list.range_end += amount;
    }
}

void SlidingSync::scrollUp(int amount) {
    for (auto& list : _lists) {
        list.range_start = std::max(0, list.range_start - amount);
        list.range_end = std::max(list.range_start + (list.range_end - list.range_start),
                                  list.range_end - amount);
    }
}

SlidingSyncResponse SlidingSync::syncOnce(int timeout_ms) {
    SlidingSyncRequest req;
    req.lists = _lists;
    req.room_subscriptions = _subscriptions;
    req.pos = _pos;
    req.timeout_ms = timeout_ms;

    http::Client http;
    http.setTimeout(timeout_ms / 1000 + 10);
    http::Response resp = http.post(
        _homeserver + "/_matrix/client/unstable/org.matrix.msc3575/sync",
        req.toJson().dump(),
        {
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + _token}
        }
    );

    if (!resp.ok()) throw std::runtime_error("Sliding sync HTTP " + std::to_string(resp.status_code));

    SlidingSyncResponse sr;
    if (!sr.fromJson(resp.body)) throw std::runtime_error("Sliding sync parse error");

    _pos = sr.pos;
    return sr;
}

}} // namespace matrixcli::matrix

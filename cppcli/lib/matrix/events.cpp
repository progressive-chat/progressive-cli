#include "events.hpp"
#include <sstream>

namespace matrixcli { namespace matrix {

Event Event::fromJson(const json& j) {
    Event ev;
    ev.type = j.value("type", "");
    ev.event_id = j.value("event_id", "");
    ev.sender = j.value("sender", "");
    ev.room_id = j.value("room_id", "");
    ev.state_key = j.value("state_key", "");
    ev.origin_server_ts = j.value("origin_server_ts", 0L);
    ev.content = j.value("content", json::object());
    ev.prev_content = j.value("prev_content", json::object());
    ev.redacts = j.value("redacts", "");

    if (j.contains("unsigned")) {
        auto& u = j["unsigned"];
        ev.unsigned_data.age = u.value("age", 0L);
        ev.unsigned_data.transaction_id = u.value("transaction_id", "");
        if (u.contains("redacted_because")) {
            auto& rb = u["redacted_because"];
            ev.unsigned_data.redacted_because = rb.value("event_id", "");
        }
    }

    return ev;
}

json Event::toJson() const {
    json j;
    j["type"] = type;
    j["event_id"] = event_id;
    j["sender"] = sender;
    j["room_id"] = room_id;
    if (!state_key.empty()) j["state_key"] = state_key;
    j["origin_server_ts"] = origin_server_ts;
    j["content"] = content;
    if (!prev_content.empty()) j["prev_content"] = prev_content;
    if (!redacts.empty()) j["redacts"] = redacts;

    json u;
    if (unsigned_data.age > 0) u["age"] = unsigned_data.age;
    if (!unsigned_data.transaction_id.empty()) u["transaction_id"] = unsigned_data.transaction_id;
    if (!unsigned_data.redacted_because.empty())
        u["redacted_because"] = {{"event_id", unsigned_data.redacted_because}};
    if (!u.empty()) j["unsigned"] = u;

    return j;
}

SyncRoom SyncRoom::fromJson(const json& j) {
    SyncRoom room;

    if (j.contains("state") && j["state"].contains("events")) {
        for (auto& ev : j["state"]["events"])
            room.state.events.push_back(Event::fromJson(ev));
    }

    if (j.contains("timeline")) {
        auto& tl = j["timeline"];
        room.timeline.limited = tl.value("limited", false);
        room.timeline.prev_batch = tl.value("prev_batch", "");
        if (tl.contains("events")) {
            for (auto& ev : tl["events"])
                room.timeline.events.push_back(Event::fromJson(ev));
        }
    }

    if (j.contains("ephemeral") && j["ephemeral"].contains("events")) {
        for (auto& ev : j["ephemeral"]["events"])
            room.ephemeral.events.push_back(Event::fromJson(ev));
    }

    if (j.contains("account_data") && j["account_data"].contains("events")) {
        for (auto& ev : j["account_data"]["events"])
            room.account_data.events.push_back(Event::fromJson(ev));
    }

    if (j.contains("unread_notifications")) {
        auto& un = j["unread_notifications"];
        room.notification_count = un.value("notification_count", 0);
        room.highlight_count = un.value("highlight_count", 0);
    }

    if (j.contains("summary")) {
        auto& s = j["summary"];
        if (s.contains("m.heroes")) {
            for (auto& h : s["m.heroes"])
                room.heroes.push_back(h.get<std::string>());
        }
        room.joined_member_count = s.value("m.joined_member_count", 0);
        room.invited_member_count = s.value("m.invited_member_count", 0);
    }

    return room;
}

SyncInvitedRoom SyncInvitedRoom::fromJson(const json& j) {
    SyncInvitedRoom room;
    if (j.contains("invite_state") && j["invite_state"].contains("events")) {
        for (auto& ev : j["invite_state"]["events"])
            room.invite_state.push_back(Event::fromJson(ev));
    }
    return room;
}

SyncResponse SyncResponse::fromJson(const std::string& json_str) {
    SyncResponse sr;
    json j = json::parse(json_str);

    sr.next_batch = j.value("next_batch", "");

    if (j.contains("account_data") && j["account_data"].contains("events")) {
        for (auto& ev : j["account_data"]["events"])
            sr.account_data.push_back(Event::fromJson(ev));
    }

    if (j.contains("presence") && j["presence"].contains("events")) {
        for (auto& ev : j["presence"]["events"])
            sr.presence.push_back(Event::fromJson(ev));
    }

    if (j.contains("to_device") && j["to_device"].contains("events")) {
        for (auto& ev : j["to_device"]["events"])
            sr.to_device.push_back(Event::fromJson(ev));
    }

    if (j.contains("device_lists")) {
        auto& dl = j["device_lists"];
        if (dl.contains("changed")) {
            for (auto& id : dl["changed"])
                sr.device_list_changed.push_back(id.get<std::string>());
        }
        if (dl.contains("left")) {
            for (auto& id : dl["left"])
                sr.device_list_left.push_back(id.get<std::string>());
        }
    }

    if (j.contains("device_one_time_keys_count")) {
        sr.signed_curve25519_count = j["device_one_time_keys_count"].value("signed_curve25519", 0);
    }

    if (j.contains("rooms")) {
        auto& rooms = j["rooms"];
        if (rooms.contains("join")) {
            for (auto& [room_id, room_json] : rooms["join"].items()) {
                sr.rooms.join[room_id] = SyncRoom::fromJson(room_json);
            }
        }
        if (rooms.contains("invite")) {
            for (auto& [room_id, room_json] : rooms["invite"].items()) {
                sr.rooms.invite[room_id] = SyncInvitedRoom::fromJson(room_json);
            }
        }
        if (rooms.contains("leave")) {
            for (auto& [room_id, room_json] : rooms["leave"].items()) {
                sr.rooms.leave[room_id] = SyncRoom::fromJson(room_json);
            }
        }
    }

    return sr;
}

UserInfo UserInfo::fromJson(const json& j) {
    UserInfo info;
    info.user_id = j.value("user_id", "");
    info.display_name = j.value("displayname", "");
    info.avatar_url = j.value("avatar_url", "");
    return info;
}

MessageContent MessageContent::fromJson(const json& j) {
    MessageContent mc;
    mc.msgtype = j.value("msgtype", "");
    mc.body = j.value("body", "");
    mc.formatted_body = j.value("formatted_body", "");
    mc.format = j.value("format", "");
    mc.url = j.value("url", "");

    if (j.contains("info")) {
        auto& info = j["info"];
        mc.info_name = info.value("name", "");
        mc.info_size = info.value("size", 0L);
        mc.info_mimetype = info.value("mimetype", "");
    }

    return mc;
}

}} // namespace matrixcli::matrix

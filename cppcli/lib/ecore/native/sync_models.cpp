#include "progressive/sync_models.hpp"
#include <cstring>

namespace progressive {

// ==== JSON Helpers ====

static std::string extractJsonString(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    size_t end = pos;
    while (end < json.size() && json[end] != '"') {
        if (json[end] == '\\') end++;
        end++;
    }
    return json.substr(pos, end - pos);
}

static int extractJsonInt(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return 0;
    int val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return val;
}

static bool extractJsonBool(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    return json.compare(pos, 4, "true") == 0;
}

static std::string extractJsonObject(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '{') return "";
    int depth = 1;
    size_t start = pos;
    pos++;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') depth--;
        pos++;
    }
    return json.substr(start, pos - start);
}

static std::vector<std::string> extractJsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;
    pos++;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ',' || json[pos] == '\n')) pos++;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] == '"') {
            pos++;
            size_t end = pos;
            while (end < json.size() && json[end] != '"') end++;
            result.push_back(json.substr(pos, end - pos));
            pos = end + 1;
        } else {
            pos++;
        }
    }
    return result;
}

static std::vector<Event> extractJsonEventsArray(const std::string& json, const std::string& key) {
    std::vector<Event> result;
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return result;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return result;
    pos++;
    while (pos < json.size()) {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ',' || json[pos] == '\n')) pos++;
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] == '{') {
            int depth = 1;
            size_t start = pos;
            pos++;
            while (pos < json.size() && depth > 0) {
                if (json[pos] == '{') depth++;
                else if (json[pos] == '}') depth--;
                pos++;
            }
            std::string eventJson = json.substr(start, pos - start);
            result.push_back(parseEvent(eventJson));
        } else {
            pos++;
        }
    }
    return result;
}

// ==== Parse SyncSection ====

SyncDeviceListResponse parseSyncDeviceList(const std::string& json) {
    SyncDeviceListResponse r;
    r.changed = extractJsonStringArray(json, "changed");
    r.left = extractJsonStringArray(json, "left");
    return r;
}

SyncRoomTimeline parseSyncTimeline(const std::string& json) {
    SyncRoomTimeline t;
    t.events = extractJsonEventsArray(json, "events");
    t.limited = extractJsonBool(json, "limited");
    t.prevToken = extractJsonString(json, "prev_batch");
    return t;
}

SyncUnreadNotifications parseSyncUnreadNotifications(const std::string& json) {
    SyncUnreadNotifications u;
    u.notificationCount = extractJsonInt(json, "notification_count");
    u.highlightCount = extractJsonInt(json, "highlight_count");
    return u;
}

SyncRoomSummary parseSyncRoomSummary(const std::string& json) {
    SyncRoomSummary s;
    s.heroes = extractJsonStringArray(json, "m.heroes");
    s.joinedMembersCount = extractJsonInt(json, "m.joined_member_count");
    s.invitedMembersCount = extractJsonInt(json, "m.invited_member_count");
    return s;
}

// ==== Parse RoomSync ====
//
// Original Kotlin (RoomSync.kt:26-54):
//   data class RoomSync(state, timeline, ephemeral, accountData,
//       unreadNotifications, unreadThreadNotifications, summary)

SyncRoom parseSyncRoom(const std::string& json) {
    SyncRoom room;

    auto stateJson = extractJsonObject(json, "state");
    if (!stateJson.empty()) {
        room.state.events = extractJsonEventsArray(stateJson, "events");
    }

    auto timelineJson = extractJsonObject(json, "timeline");
    if (!timelineJson.empty()) {
        room.timeline = parseSyncTimeline(timelineJson);
    }

    // Ephemeral — store as lazy (raw JSON), parse on demand
    auto ephemeralJson = extractJsonObject(json, "ephemeral");
    if (!ephemeralJson.empty()) {
        room.ephemeral.storedJson = ephemeralJson;
        room.ephemeral.state = EphemeralState::STORED;
    }

    auto acctJson = extractJsonObject(json, "account_data");
    if (!acctJson.empty()) {
        room.accountData.events = extractJsonEventsArray(acctJson, "events");
    }

    auto unreadJson = extractJsonObject(json, "unread_notifications");
    if (!unreadJson.empty()) {
        room.unreadNotifications = parseSyncUnreadNotifications(unreadJson);
    }

    auto summaryJson = extractJsonObject(json, "summary");
    if (!summaryJson.empty()) {
        room.summary = parseSyncRoomSummary(summaryJson);
    }

    return room;
}

// ==== Parse RoomsSyncResponse ====
//
// Original Kotlin (RoomsSyncResponse.kt:25-39):
//   data class RoomsSyncResponse(join, invite, leave)
//
// JSON: {"join":{"!room:server":{...}},"invite":{...},"leave":{...}}

SyncRoomsResponse parseSyncRooms(const std::string& json) {
    SyncRoomsResponse rooms;

    // Parse "join" map
    auto joinJson = extractJsonObject(json, "join");
    if (!joinJson.empty()) {
        size_t pos = 1;
        while (pos < joinJson.size()) {
            while (pos < joinJson.size() && (joinJson[pos] == ' ' || joinJson[pos] == ',' || joinJson[pos] == '\n')) pos++;
            if (pos >= joinJson.size() || joinJson[pos] == '}') break;
            if (joinJson[pos] == '"') {
                pos++;
                size_t keyEnd = pos;
                while (keyEnd < joinJson.size() && joinJson[keyEnd] != '"') keyEnd++;
                std::string roomId = joinJson.substr(pos, keyEnd - pos);
                pos = keyEnd + 1;
                while (pos < joinJson.size() && joinJson[pos] != ':') pos++;
                pos++;
                while (pos < joinJson.size() && (joinJson[pos] == ' ' || joinJson[pos] == '\n')) pos++;
                if (pos < joinJson.size() && joinJson[pos] == '{') {
                    int depth = 1;
                    size_t start = pos;
                    pos++;
                    while (pos < joinJson.size() && depth > 0) {
                        if (joinJson[pos] == '{') depth++;
                        else if (joinJson[pos] == '}') depth--;
                        pos++;
                    }
                    rooms.join[roomId] = parseSyncRoom(joinJson.substr(start, pos - start));
                }
            }
        }
    }

    // Parse "invite" map
    auto inviteJson = extractJsonObject(json, "invite");
    if (!inviteJson.empty()) {
        size_t pos = 1;
        while (pos < inviteJson.size()) {
            while (pos < inviteJson.size() && (inviteJson[pos] == ' ' || inviteJson[pos] == ',' || inviteJson[pos] == '\n')) pos++;
            if (pos >= inviteJson.size() || inviteJson[pos] == '}') break;
            if (inviteJson[pos] == '"') {
                pos++;
                size_t keyEnd = pos;
                while (keyEnd < inviteJson.size() && inviteJson[keyEnd] != '"') keyEnd++;
                std::string roomId = inviteJson.substr(pos, keyEnd - pos);
                pos = keyEnd + 1;
                while (pos < inviteJson.size() && inviteJson[pos] != ':') pos++;
                pos++;
                while (pos < inviteJson.size() && (inviteJson[pos] == ' ' || inviteJson[pos] == '\n')) pos++;
                if (pos < inviteJson.size() && inviteJson[pos] == '{') {
                    int depth = 1;
                    size_t start = pos;
                    pos++;
                    while (pos < inviteJson.size() && depth > 0) {
                        if (inviteJson[pos] == '{') depth++;
                        else if (inviteJson[pos] == '}') depth--;
                        pos++;
                    }
                    std::string invJson = inviteJson.substr(start, pos - start);
                    SyncInvitedRoom inv;
                    auto isJson = extractJsonObject(invJson, "invite_state");
                    if (!isJson.empty()) {
                        inv.inviteState.events = extractJsonEventsArray(isJson, "events");
                    }
                    rooms.invite[roomId] = inv;
                }
            }
        }
    }

    // Parse "leave" map
    auto leaveJson = extractJsonObject(json, "leave");
    if (!leaveJson.empty()) {
        size_t pos = 1;
        while (pos < leaveJson.size()) {
            while (pos < leaveJson.size() && (leaveJson[pos] == ' ' || leaveJson[pos] == ',' || leaveJson[pos] == '\n')) pos++;
            if (pos >= leaveJson.size() || leaveJson[pos] == '}') break;
            if (leaveJson[pos] == '"') {
                pos++;
                size_t keyEnd = pos;
                while (keyEnd < leaveJson.size() && leaveJson[keyEnd] != '"') keyEnd++;
                std::string roomId = leaveJson.substr(pos, keyEnd - pos);
                pos = keyEnd + 1;
                while (pos < leaveJson.size() && leaveJson[pos] != ':') pos++;
                pos++;
                while (pos < leaveJson.size() && (leaveJson[pos] == ' ' || leaveJson[pos] == '\n')) pos++;
                if (pos < leaveJson.size() && leaveJson[pos] == '{') {
                    int depth = 1;
                    size_t start = pos;
                    pos++;
                    while (pos < leaveJson.size() && depth > 0) {
                        if (leaveJson[pos] == '{') depth++;
                        else if (leaveJson[pos] == '}') depth--;
                        pos++;
                    }
                    rooms.leave[roomId] = parseSyncRoom(leaveJson.substr(start, pos - start));
                }
            }
        }
    }

    return rooms;
}

// ==== Parse Top-Level SyncResponse ====
//
// Original Kotlin (SyncResponse.kt:25-64)

SyncResponse parseSyncResponse(const std::string& json) {
    SyncResponse response;

    // Original Kotlin: account_data
    auto acctJson = extractJsonObject(json, "account_data");
    if (!acctJson.empty()) {
        response.accountData.events = extractJsonEventsArray(acctJson, "events");
    }

    // Original Kotlin: next_batch
    response.nextBatch = extractJsonString(json, "next_batch");

    // Original Kotlin: presence
    auto presJson = extractJsonObject(json, "presence");
    if (!presJson.empty()) {
        response.presence.events = extractJsonEventsArray(presJson, "events");
    }

    // Original Kotlin: to_device
    auto tdJson = extractJsonObject(json, "to_device");
    if (!tdJson.empty()) {
        response.toDevice.events = extractJsonEventsArray(tdJson, "events");
    }

    // Original Kotlin: rooms
    auto roomsJson = extractJsonObject(json, "rooms");
    if (!roomsJson.empty()) {
        response.rooms = parseSyncRooms(roomsJson);
    }

    // Original Kotlin: device_lists
    auto dlJson = extractJsonObject(json, "device_lists");
    if (!dlJson.empty()) {
        response.deviceLists = parseSyncDeviceList(dlJson);
    }

    // Original Kotlin: device_one_time_keys_count
    auto otkJson = extractJsonObject(json, "device_one_time_keys_count");
    if (!otkJson.empty()) {
        response.deviceOneTimeKeysCount.signedCurve25519 = extractJsonInt(otkJson, "signed_curve25519");
    }

    // Original Kotlin: device_unused_fallback_key_types
    response.deviceUnusedFallbackKeyTypes = extractJsonStringArray(json, "device_unused_fallback_key_types");

    return response;
}

// ==== Serialize SyncResponse ====

std::string syncResponseToJson(const SyncResponse& response) {
    std::string json = "{";
    json += "\"next_batch\":\"" + response.nextBatch + "\"";
    // Minimal serialization — full serialization is complex
    // Used primarily for caching the next_batch token
    json += "}";
    return json;
}

} // namespace progressive

#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace matrixcli { namespace matrix {

using json = nlohmann::json;

enum class EventType {
    Unknown,
    Message,
    StateMember,
    StateName,
    StateTopic,
    StateAvatar,
    StateCreate,
    StateJoinRules,
    StatePowerLevels,
    StateEncryption,
    StateCanonicalAlias,
    StateHistoryVisibility,
    StateTombstone,
    StatePinnedEvents,
    StateAliases,
    Typing,
    Receipt,
    Redaction,
    Reaction,
    Encrypted,
    Sticker,
    RoomKey,
    RoomKeyRequest,
    ForwardedRoomKey,
    PollStart,
    PollResponse,
    PollEnd,
    KeyVerificationRequest,
    KeyVerificationStart,
    KeyVerificationAccept,
    KeyVerificationKey,
    KeyVerificationMac,
    KeyVerificationCancel,
    KeyVerificationDone,
    CallInvite,
    CallAnswer,
    CallHangup,
    CallCandidates,
    Presence
};

inline EventType eventTypeFromString(const std::string& s) {
    if (s == "m.room.message") return EventType::Message;
    if (s == "m.room.member") return EventType::StateMember;
    if (s == "m.room.name") return EventType::StateName;
    if (s == "m.room.topic") return EventType::StateTopic;
    if (s == "m.room.avatar") return EventType::StateAvatar;
    if (s == "m.room.create") return EventType::StateCreate;
    if (s == "m.room.join_rules") return EventType::StateJoinRules;
    if (s == "m.room.power_levels") return EventType::StatePowerLevels;
    if (s == "m.room.encryption") return EventType::StateEncryption;
    if (s == "m.room.canonical_alias") return EventType::StateCanonicalAlias;
    if (s == "m.room.history_visibility") return EventType::StateHistoryVisibility;
    if (s == "m.room.tombstone") return EventType::StateTombstone;
    if (s == "m.room.pinned_events") return EventType::StatePinnedEvents;
    if (s == "m.room.aliases") return EventType::StateAliases;
    if (s == "m.typing") return EventType::Typing;
    if (s == "m.receipt") return EventType::Receipt;
    if (s == "m.room.redaction") return EventType::Redaction;
    if (s == "m.reaction") return EventType::Reaction;
    if (s == "m.room.encrypted") return EventType::Encrypted;
    if (s == "m.sticker") return EventType::Sticker;
    if (s == "m.room_key") return EventType::RoomKey;
    if (s == "m.room_key_request") return EventType::RoomKeyRequest;
    if (s == "m.forwarded_room_key") return EventType::ForwardedRoomKey;
    if (s == "m.poll.start") return EventType::PollStart;
    if (s == "m.poll.response") return EventType::PollResponse;
    if (s == "m.poll.end") return EventType::PollEnd;
    if (s == "m.key.verification.request") return EventType::KeyVerificationRequest;
    if (s == "m.key.verification.start") return EventType::KeyVerificationStart;
    if (s == "m.key.verification.accept") return EventType::KeyVerificationAccept;
    if (s == "m.key.verification.key") return EventType::KeyVerificationKey;
    if (s == "m.key.verification.mac") return EventType::KeyVerificationMac;
    if (s == "m.key.verification.cancel") return EventType::KeyVerificationCancel;
    if (s == "m.key.verification.done") return EventType::KeyVerificationDone;
    if (s == "m.call.invite") return EventType::CallInvite;
    if (s == "m.call.answer") return EventType::CallAnswer;
    if (s == "m.call.hangup") return EventType::CallHangup;
    if (s == "m.call.candidates") return EventType::CallCandidates;
    if (s == "m.presence") return EventType::Presence;
    return EventType::Unknown;
}

inline std::string eventTypeToString(EventType t) {
    switch (t) {
        case EventType::Message: return "m.room.message";
        case EventType::StateMember: return "m.room.member";
        case EventType::StateName: return "m.room.name";
        case EventType::StateTopic: return "m.room.topic";
        case EventType::StateAvatar: return "m.room.avatar";
        case EventType::StateCreate: return "m.room.create";
        case EventType::StateJoinRules: return "m.room.join_rules";
        case EventType::StatePowerLevels: return "m.room.power_levels";
        case EventType::StateEncryption: return "m.room.encryption";
        case EventType::StateCanonicalAlias: return "m.room.canonical_alias";
        case EventType::StateHistoryVisibility: return "m.room.history_visibility";
        case EventType::StateTombstone: return "m.room.tombstone";
        case EventType::StatePinnedEvents: return "m.room.pinned_events";
        case EventType::StateAliases: return "m.room.aliases";
        case EventType::Typing: return "m.typing";
        case EventType::Receipt: return "m.receipt";
        case EventType::Redaction: return "m.room.redaction";
        case EventType::Reaction: return "m.reaction";
        case EventType::Encrypted: return "m.room.encrypted";
        case EventType::Sticker: return "m.sticker";
        case EventType::RoomKey: return "m.room_key";
        case EventType::RoomKeyRequest: return "m.room_key_request";
        case EventType::ForwardedRoomKey: return "m.forwarded_room_key";
        case EventType::PollStart: return "m.poll.start";
        case EventType::PollResponse: return "m.poll.response";
        case EventType::PollEnd: return "m.poll.end";
        case EventType::KeyVerificationRequest: return "m.key.verification.request";
        case EventType::KeyVerificationStart: return "m.key.verification.start";
        case EventType::KeyVerificationAccept: return "m.key.verification.accept";
        case EventType::KeyVerificationKey: return "m.key.verification.key";
        case EventType::KeyVerificationMac: return "m.key.verification.mac";
        case EventType::KeyVerificationCancel: return "m.key.verification.cancel";
        case EventType::KeyVerificationDone: return "m.key.verification.done";
        case EventType::CallInvite: return "m.call.invite";
        case EventType::CallAnswer: return "m.call.answer";
        case EventType::CallHangup: return "m.call.hangup";
        case EventType::CallCandidates: return "m.call.candidates";
        case EventType::Presence: return "m.presence";
        default: return "unknown";
    }
}

struct UnsignedData {
    int64_t age = 0;
    std::string transaction_id;
    std::string redacted_because;

    bool is_redacted() const { return !redacted_because.empty(); }
};

struct Event {
    std::string type;
    std::string event_id;
    std::string sender;
    std::string room_id;
    std::string state_key;
    int64_t origin_server_ts = 0;
    json content;
    json prev_content;
    UnsignedData unsigned_data;
    std::string redacts;

    EventType event_type() const { return eventTypeFromString(type); }
    bool is_state() const { return !state_key.empty(); }

    static Event fromJson(const json& j);
    json toJson() const;
};

struct SyncTimeline {
    std::vector<Event> events;
    bool limited = false;
    std::string prev_batch;
};

struct SyncState {
    std::vector<Event> events;
};

struct SyncRoom {
    SyncState state;
    SyncTimeline timeline;
    SyncState ephemeral;
    SyncState account_data;
    int notification_count = 0;
    int highlight_count = 0;
    std::vector<std::string> heroes;
    int joined_member_count = 0;
    int invited_member_count = 0;

    static SyncRoom fromJson(const json& j);
};

struct SyncInvitedRoom {
    std::vector<Event> invite_state;

    static SyncInvitedRoom fromJson(const json& j);
};

struct SyncRooms {
    std::map<std::string, SyncRoom> join;
    std::map<std::string, SyncInvitedRoom> invite;
    std::map<std::string, SyncRoom> leave;
};

struct SyncResponse {
    std::string next_batch;
    std::vector<Event> account_data;
    std::vector<Event> presence;
    std::vector<Event> to_device;
    SyncRooms rooms;
    std::vector<std::string> device_list_changed;
    std::vector<std::string> device_list_left;
    int signed_curve25519_count = 0;

    static SyncResponse fromJson(const std::string& json_str);
};

struct RoomInfo {
    std::string room_id;
    std::string name;
    std::string topic;
    std::string avatar_url;
    std::string canonical_alias;
    std::vector<std::string> aliases;
    std::string join_rule;
    int version = 0;
    bool encrypted = false;
    int64_t member_count = 0;
};

struct UserInfo {
    std::string user_id;
    std::string display_name;
    std::string avatar_url;

    static UserInfo fromJson(const json& j);
};

// Message content helpers
struct MessageContent {
    std::string msgtype;
    std::string body;
    std::string formatted_body;
    std::string format;
    std::string url;
    std::string info_name;
    int64_t info_size = 0;
    std::string info_mimetype;

    static MessageContent fromJson(const json& j);
};

}} // namespace matrixcli::matrix

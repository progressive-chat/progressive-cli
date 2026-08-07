#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "progressive/event_models.hpp"

namespace progressive {

// ==== Sync Models — Matrix /sync v2 API Response ====
//
// Ref: https://matrix.org/docs/spec/client_server/latest#get-matrix-client-r0-sync
//
// These structs mirror the Kotlin data classes in api/session/sync/model/

// Original Kotlin (DeviceListResponse.kt:25-30):
//   data class DeviceListResponse(changed: List<String>, left: List<String>)
struct SyncDeviceListResponse {
    std::vector<std::string> changed;  // user IDs with new crypto devices
    std::vector<std::string> left;     // user IDs no longer tracked
};

// Original Kotlin (DeviceOneTimeKeysCountSyncResponse.kt:25-27):
//   data class DeviceOneTimeKeysCountSyncResponse(signedCurve25519: Int?)
struct SyncDeviceOneTimeKeysCount {
    int signedCurve25519 = 0;
};

// Original Kotlin (PresenceSyncResponse.kt:26-32):
//   data class PresenceSyncResponse(events: List<Event>?)
struct SyncPresenceResponse {
    std::vector<Event> events;
};

// Original Kotlin (UserAccountDataSync.kt:25-27):
//   data class UserAccountDataSync(list: List<UserAccountDataEvent>)
struct SyncUserAccountData {
    std::vector<Event> events;  // account data events
};

// Original Kotlin (ToDeviceSyncResponse.kt:24-30):
//   data class ToDeviceSyncResponse(events: List<Event>?)
struct SyncToDeviceResponse {
    std::vector<Event> events;
};

// Original Kotlin (RoomSyncUnreadNotifications.kt:28-42):
//   data class RoomSyncUnreadNotifications(events, notificationCount, highlightCount)
struct SyncUnreadNotifications {
    std::vector<Event> events;
    int notificationCount = 0;
    int highlightCount = 0;
};

// Original Kotlin (RoomSyncUnreadThreadNotifications.kt:25-33):
//   data class RoomSyncUnreadThreadNotifications(notificationCount, highlightCount)
struct SyncUnreadThreadNotifications {
    int notificationCount = 0;
    int highlightCount = 0;
};

// Original Kotlin (RoomSyncSummary.kt:26-46):
//   data class RoomSyncSummary(heroes, joinedMembersCount, invitedMembersCount)
struct SyncRoomSummary {
    std::vector<std::string> heroes;       // "m.heroes" key
    int joinedMembersCount = 0;            // "m.joined_member_count"
    int invitedMembersCount = 0;           // "m.invited_member_count"
};

// Original Kotlin (RoomSyncTimeline.kt:25-37):
//   data class RoomSyncTimeline(events, limited, prevToken)
struct SyncRoomTimeline {
    std::vector<Event> events;
    bool limited = false;                  // "limited" key
    std::string prevToken;                 // "prev_batch" key — pagination token
};

// Original Kotlin (RoomSyncState.kt:25-31):
//   data class RoomSyncState(events: List<Event>?)
struct SyncRoomState {
    std::vector<Event> events;
};

// Original Kotlin (RoomSyncEphemeral.kt:25-31):
//   data class RoomSyncEphemeral(events: List<Event>?)
struct SyncRoomEphemeral {
    std::vector<Event> events;             // typing, receipts
};

// Original Kotlin (RoomSyncAccountData.kt:26-31):
//   data class RoomSyncAccountData(events: List<Event>?)
struct SyncRoomAccountData {
    std::vector<Event> events;             // tags, etc.
};

// Original Kotlin (LazyRoomSyncEphemeral.kt:22-25):
//   sealed class LazyRoomSyncEphemeral {
//       data class Parsed(val roomSyncEphemeral: RoomSyncEphemeral)
//       object Stored
//   }
enum class EphemeralState {
    STORED = 0,     // Not parsed yet, stored as raw JSON
    PARSED = 1      // Already parsed into SyncRoomEphemeral
};

struct SyncLazyEphemeral {
    EphemeralState state = EphemeralState::STORED;
    SyncRoomEphemeral parsed;              // Only valid if state == PARSED
    std::string storedJson;                // Raw JSON if state == STORED
};

// Original Kotlin (RoomSync.kt:26-54):
//   data class RoomSync(state, timeline, ephemeral, accountData,
//       unreadNotifications, unreadThreadNotifications, summary)
struct SyncRoom {
    SyncRoomState state;                   // "state" key
    SyncRoomTimeline timeline;             // "timeline" key
    SyncLazyEphemeral ephemeral;           // "ephemeral" key (lazy)
    SyncRoomAccountData accountData;       // "account_data" key
    SyncUnreadNotifications unreadNotifications;     // "unread_notifications"
    std::unordered_map<std::string, SyncUnreadThreadNotifications> unreadThreadNotifications; // "unread_thread_notifications"
    SyncRoomSummary summary;               // "summary" key
};

// Original Kotlin (RoomInviteState.kt:25-30):
//   data class RoomInviteState(events: List<Event>)
struct SyncRoomInviteState {
    std::vector<Event> events;
};

// Original Kotlin (InvitedRoomSync.kt:25-35):
//   data class InvitedRoomSync(inviteState: RoomInviteState?)
struct SyncInvitedRoom {
    SyncRoomInviteState inviteState;       // "invite_state" key
};

// Original Kotlin (RoomsSyncResponse.kt:25-39):
//   data class RoomsSyncResponse(join, invite, leave)
struct SyncRoomsResponse {
    // key = room_id, value = room sync data
    std::unordered_map<std::string, SyncRoom> join;
    std::unordered_map<std::string, SyncInvitedRoom> invite;
    std::unordered_map<std::string, SyncRoom> leave;
};

// ==== Top-Level Sync Response ====
//
// Original Kotlin (SyncResponse.kt:25-64):
//   data class SyncResponse(
//       accountData, nextBatch, presence, toDevice,
//       rooms, deviceLists, deviceOneTimeKeysCount,
//       devDeviceUnusedFallbackKeyTypes, stableDeviceUnusedFallbackKeyTypes
//   )

struct SyncResponse {
    SyncUserAccountData accountData;        // "account_data" key
    std::string nextBatch;                  // "next_batch" key — pagination token
    SyncPresenceResponse presence;          // "presence" key
    SyncToDeviceResponse toDevice;          // "to_device" key
    SyncRoomsResponse rooms;                // "rooms" key
    SyncDeviceListResponse deviceLists;     // "device_lists" key
    SyncDeviceOneTimeKeysCount deviceOneTimeKeysCount; // "device_one_time_keys_count"
    std::vector<std::string> deviceUnusedFallbackKeyTypes; // "device_unused_fallback_key_types"

    bool empty() const { return nextBatch.empty() && rooms.join.empty()
        && rooms.invite.empty() && rooms.leave.empty(); }
};

// ==== JSON Parsing ====

SyncResponse parseSyncResponse(const std::string& json);
SyncRoomsResponse parseSyncRooms(const std::string& json);
SyncRoom parseSyncRoom(const std::string& json);
SyncRoomTimeline parseSyncTimeline(const std::string& json);
SyncUnreadNotifications parseSyncUnreadNotifications(const std::string& json);
SyncRoomSummary parseSyncRoomSummary(const std::string& json);
SyncDeviceListResponse parseSyncDeviceList(const std::string& json);

// Serialize top-level sync response (for caching)
std::string syncResponseToJson(const SyncResponse& response);

// ==== Sync State Machine ====
//
// Original Kotlin (SyncState.kt:24-27), (SyncRequestState.kt:20-44), (InitialSyncStrategy.kt:22-53)

enum class SyncStateType { IDLE = 0, RUNNING = 1, PAUSED = 2, KILLING = 3, KILLED = 4, NO_NETWORK = 5, INVALID_TOKEN = 6 };

struct SyncState {
    SyncStateType type = SyncStateType::IDLE;
    bool afterPause = false;             // for RUNNING state
};

enum class InitialSyncStepType { DOWNLOADING = 0, IMPORTING_ACCOUNT = 1, IMPORTING_DEFERRED_KEYS = 2 };

struct InitialSyncStep {
    InitialSyncStepType type = InitialSyncStepType::DOWNLOADING;
    int percentProgress = 0;
};

struct IncrementalSyncParsing {
    int rooms = 0;
    int toDevice = 0;
};

enum class SyncRequestStateType { IDLE = 0, INITIAL_PROGRESS = 1, INCREMENTAL_IDLE = 2, INCREMENTAL_PARSING = 3, INCREMENTAL_ERROR = 4, INCREMENTAL_DONE = 5 };

struct SyncRequestState {
    SyncRequestStateType type = SyncRequestStateType::IDLE;
    InitialSyncStep initialStep;         // for INITIAL_PROGRESS
    IncrementalSyncParsing parsing;       // for INCREMENTAL_PARSING
};

enum class InitialSyncStrategyType { LEGACY = 0, OPTIMIZED = 1 };

struct InitialSyncStrategy {
    InitialSyncStrategyType type = InitialSyncStrategyType::OPTIMIZED;
    int64_t minSizeToSplit = 1048576;    // 1 MB
    int64_t minSizeToStoreInFile = 1024; // 1 KB
    int maxRoomsToInsert = 100;
};

} // namespace progressive

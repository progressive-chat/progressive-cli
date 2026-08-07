// src/core/engine/sync_applier.hpp — pure sync->state application (X1 phase 3).
// Runs on the WORKER thread: produces the RoomSyncUpdate delta from a
// FastSyncResponse. The UI applies the delta (applyRoomSyncUpdate) — no
// logic may drift back into the UI; the UI never re-derives sync state.
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine_types.hpp"
#include "../fast_sync.hpp"

namespace progressive::desktop {

class Decryptor;

struct RoomMeta {
    std::string name;
    std::string avatarUrl;
    std::string dmDisplayName;
    std::string dmAvatarUrl;
    std::string canonicalAlias;
    bool isEncrypted = false;
};

// Prepared update — computed on the worker thread, applied on the UI thread.
// Qt-free: the UI builds the invite label from inviteCount.
struct RoomSyncUpdate {
    std::vector<RoomData> roomsToUpsert;
    std::vector<std::string> roomsToRemove;
    std::vector<RoomData> invitedRooms;
    int inviteCount = 0;
    bool currentRoomUpdated = false;
    std::string currentRoomId;
    std::vector<FastEvent> currentRoomEvents;
    std::unordered_map<std::string, std::string> currentRoomAvatars;
    std::unordered_map<std::string, std::string> currentRoomMemberNames;
    std::string lastNotificationBody;
};

class SyncApplier {
public:
    // Build the delta (worker thread — no model access).
    static RoomSyncUpdate prepareRoomSyncUpdate(const FastSyncResponse& resp,
                                                const std::string& currentRoomId,
                                                const std::string& myUserId);

    // Decrypt + convert ONE re-decrypted event (worker or UI thread, pure).
    static bool decryptEventToDisplayed(const FastEvent& fe, DisplayedEvent& de,
                                        const std::string& currentRoomId,
                                        Decryptor* decryptor);

    // Pure helpers (moved from room_store).
    static RoomMeta extractRoomMeta(const FastRoom& room, const std::string& myUserId);
    static std::string extractLastMessageBody(const std::vector<FastEvent>& events);
    static std::string makeSystemBody(const std::string& type,
                                      const std::string& contentJson,
                                      const std::string& stateKey);
    static std::string extractReplyToId(std::string_view contentJson);
    static std::string extractThreadRootId(std::string_view json);
    static std::string msgType(std::string_view json);
    static std::string msgBody(std::string_view json);
    // Generic JSON string extractors (shared with the UI's appendTimelineForRoom).
    static std::string extractStringDec(std::string_view json, const std::string& key);
    // Escape-aware string-value extraction starting at an offset (used for the
    // nested {"formatted_body":{"body":"..."}} shape).
    static std::string extractStringDecAt(std::string_view json, const std::string& key,
                                          size_t startPos);
    // Strip Element's "> <@user:server> text\n\nreal" fallback from reply bodies.
    static void stripReplyFallback(DisplayedEvent& de);
    static std::string extractString(std::string_view json, const std::string& key);
    // Fill the message fields of a DisplayedEvent from a FastEvent.
    static void fastEventToDisplayed(const FastEvent& e, DisplayedEvent& de,
                                     const std::string& currentRoomId,
                                     Decryptor* decryptor);
};

} // namespace progressive::desktop

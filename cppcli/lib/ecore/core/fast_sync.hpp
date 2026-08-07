// src/core/fast_sync.hpp — zero-copy /sync parser using simdjson.
//
// Phase 3a.1 Layer 2: bypasses progressive_native's hand-rolled JSON parser
// (which does O(N) string scans per field lookup × 5000+ lookups per sync).
//
// Uses simdjson's DOM API to parse the /sync response ONCE, then walks the
// tree to build FastSyncResponse. All string fields are std::string_view
// pointing into simdjson's internal buffer (which is owned by the parser,
// which is in turn owned by FastSyncResponse).
//
// Memory: ~10MB per 5MB /sync response (5MB original string + 5MB padded
// internal simdjson buffer). progressive_native's parser creates ~15-20MB
// of intermediate string copies. ~50% reduction.
//
// Speed: simdjson parses at 1-3 GB/s on Cortex-A55. 5MB sync → ~5-10ms parse.
// progressive_native's hand-rolled parser takes ~500ms-2s on the same input.
// 50-200x speedup.

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <deque>

// Forward declaration to avoid including simdjson.h in the header (it's large).
namespace simdjson::dom { class parser; class element; }

namespace progressive::desktop {

struct FastEvent {
    std::string_view type;
    std::string_view eventId;
    std::string_view contentJson;     // view into the simdjson internal buffer
    int64_t originServerTs = 0;
    std::string_view senderId;
    std::string_view stateKey;

    bool isStateEvent() const { return !stateKey.empty(); }
    bool isMessage() const { return type == "m.room.message"; }
    bool isEncrypted() const { return type == "m.room.encrypted"; }
    bool isMemberEvent() const { return type == "m.room.member"; }
};

struct FastRoomTimeline {
    std::vector<FastEvent> events;
    bool limited = false;
    std::string_view prevToken;     // "prev_batch" key for backward pagination
};

struct FastRoom {
    std::vector<FastEvent> stateEvents;
    FastRoomTimeline timeline;
    int notificationCount = 0;
    int highlightCount = 0;
    bool isEncrypted = false;
    std::vector<std::string_view> typingUsers;  // ephemeral m.typing
};

struct InvitedRoom {
    std::string_view roomId;
    std::string_view inviterId;
    std::string_view roomName;
    std::string_view roomAvatar;
    std::string_view reason;
    bool isEncrypted = false;
    int memberCount = 0;
};

struct FastSyncResponse {
    // Shared ownership of the original JSON string + simdjson parser (whose
    // internal buffer is what all string_views point into). Both must outlive
    // the views. Uses shared_ptr so FastSyncResponse is cheaply copyable —
    // needed for thread-crossing via Qt::QueuedConnection (lambda capture).
    std::shared_ptr<std::string> buffer;
    std::shared_ptr<simdjson::dom::parser> parser;

    // Owned strings for contentJson — simdjson DOM doesn't expose raw_json()
    // so we serialize content objects via to_string(). The string_views in
    // FastEvent::contentJson point into this deque. Deque is used (not vector)
    // because deque back-inserts don't invalidate references to existing elements.
    std::shared_ptr<std::deque<std::string>> ownedContentStrings;

    std::string_view nextBatch;             // "next_batch"
    std::vector<std::pair<std::string_view, FastRoom>> joinedRooms;
    std::vector<InvitedRoom> invitedRooms;   // invites with state
    std::vector<std::string_view> leftRoomIds;
    int totalTimelineEvents = 0;
    int toDeviceEvents = 0;
    int signedCurve25519Count = -1;
    // "device_unused_fallback_key_types" — fallback key algorithms still
    // unused on the server. Absence of "signed_curve25519" = our fallback
    // was claimed (or never uploaded) → trigger a new fallback key upload.
    std::vector<std::string> unusedFallbackKeyTypes;
    std::vector<std::string> deviceListChanged;
    std::vector<std::string> deviceListLeft;

    // To-device events (Phase 4 E2EE). Each entry is a serialized JSON
    // string stored in ownedContentStrings — string_views point into the deque.
    // For m.room_key events, we extract sender (the sender's curve25519) from
    // the OUTER event, but we don't have it here. The content has the room_id,
    // session_id, session_key, and (often) the sender_key.
    std::vector<FastEvent> toDeviceEventList;

    bool empty() const {
        return nextBatch.empty() && joinedRooms.empty();
    }
};

// Parse a /sync response using simdjson. On success, returns FastSyncResponse
// with string_views valid until the response is destroyed.
// On error, returns a FastSyncResponse with empty buffer and sets errorMessage.
FastSyncResponse parseSyncResponseFast(std::string json, std::string& errorMessage,
                             const std::string& ourDeviceId = "");

// Convenience: convert a FastEvent to progressive::Event (copies strings).
// Use this only when passing to progressive_native parsers that require
// std::string. For UI display, extract fields directly via simdjson instead.
// (Defined in fast_sync.cpp — needs progressive::Event full definition.)

} // namespace progressive::desktop

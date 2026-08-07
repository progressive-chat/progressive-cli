// src/core/engine/timeline_state.hpp — Qt-free timeline state (X1 phase 2).
// Owns the canonical timeline events + the pure mutation logic (dedup,
// cap-200 eviction, thread counts, group markers, reactions). The Qt
// TimelineModel wraps this and applies the returned deltas with the
// QAbstractItemModel notifications (insert/remove/dataChanged) + scroll
// preservation — no model ops happen in this file.
//
// Thread model: mutated + read only on the worker thread today (the sync
// applier). If any future frontend touches this from another thread, add a
// mutex here — documented decision, not a hidden one.
#pragma once
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine_types.hpp"

namespace progressive::desktop {

class TimelineState {
public:
    static constexpr int MAX_TIMELINE_EVENTS = 200;

    // What changed, for the view layer to emit the right Qt ops.
    struct AppendResult {
        bool changed = false;
        int insertedRow = -1;      // single appendBack: the new row
        int firstRow = -1;         // appendBackBatch: the inserted range
        int lastRow = -1;
        int evictedCount = 0;      // cap-200 eviction from the front
        int threadRootRow = -1;    // a threadReplyCount was incremented here
    };
    enum class EchoResult { NotFound, Appended, Replaced, RemovedDuplicate };

    AppendResult appendBack(const DisplayedEvent& evt);
    AppendResult appendBackBatch(const std::vector<DisplayedEvent>& events);
    // appendFront: filtered insert at the front; returns the count inserted.
    int appendFront(const std::vector<DisplayedEvent>& evts);
    EchoResult replaceEcho(const std::string& tempEventId, const DisplayedEvent& realEvent);
    bool markDeleted(const std::string& eventId);
    bool updateBody(const std::string& eventId, const std::string& newBody);
    bool replaceEvent(const std::string& eventId, const DisplayedEvent& newEvent);
    void clear();
    bool addReaction(const std::string& eventId, const std::string& emoji,
                     const std::string& userId, const std::string& reactionEventId);
    bool removeReaction(const std::string& eventId, const std::string& emoji,
                        const std::string& userId);
    std::string myReactionId(const std::string& eventId, const std::string& emoji,
                             const std::string& myUserId) const;
    bool setPinned(const std::string& eventId, bool pinned);

    int findRow(const std::string& eventId) const;
    const DisplayedEvent* at(int row) const;
    DisplayedEvent* at(int row);
    size_t size() const { return events_.size(); }
    bool empty() const { return events_.empty(); }

    // View-grouping semantics (same-sender grouping, emoji normalization) —
    // do NOT extend with view-specific behavior here; a future UI that needs
    // different grouping recomputes its own markers.
    static void updateGroupMarkers(std::vector<DisplayedEvent>& events);
    static std::string normEmoji(const std::string& emoji);

    // The canonical data (worker-thread-only per the thread model above).
    std::vector<DisplayedEvent> events_;
    std::unordered_set<std::string> seenIds_;
    std::unordered_map<std::string, int> rowIndex_;
};

} // namespace progressive::desktop

// src/core/engine/room_state.hpp — Qt-free room-list state (X1 phase 2).
// Owns the canonical room list + ordering + hidden rooms. The Qt
// RoomListModel wraps this with the QAbstractListModel ops.
//
// Thread model: see timeline_state.hpp — worker-thread-only today; add a
// mutex here if a future frontend touches it from another thread.
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine_types.hpp"

namespace progressive::desktop {

class RoomState {
public:
    enum class UpsertResult { Hidden, Unchanged, Updated, Inserted };

    // The pure upsert: field-merge for existing rooms, sorted insert for new
    // ones (by lastActivityTs descending). Returns the affected row.
    UpsertResult upsertRoom(const RoomData& room);
    int upsertRow() const { return lastUpsertRow_; }

    void clear();
    bool removeRoom(const std::string& roomId);
    int removeRow() const { return lastRemoveRow_; }

    bool isHidden(const std::string& roomId) const { return hiddenRoomIds_.count(roomId) > 0; }
    void setHiddenRooms(std::unordered_set<std::string> ids) { hiddenRoomIds_ = std::move(ids); }
    void hideRoom(const std::string& roomId) { hiddenRoomIds_.insert(roomId); }
    void unhideRoom(const std::string& roomId) { hiddenRoomIds_.erase(roomId); }

    int findRowByRoomId(const std::string& roomId) const;
    const RoomData* at(int row) const;
    int size() const { return static_cast<int>(rooms_.size()); }
    bool empty() const { return rooms_.empty(); }
    int joinedCount() const;

    // The canonical data (worker-thread-only per the thread model).
    std::vector<RoomData> rooms_;
    std::unordered_map<std::string, int> index_;  // roomId -> row, O(1) lookup
    std::unordered_set<std::string> hiddenRoomIds_;

private:
    int lastUpsertRow_ = -1;
    int lastRemoveRow_ = -1;
};

} // namespace progressive::desktop

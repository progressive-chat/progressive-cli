// src/core/engine/room_state.cpp
#include "room_state.hpp"

#include <algorithm>

namespace progressive::desktop {

RoomState::UpsertResult RoomState::upsertRoom(const RoomData& room) {
    if (isHidden(room.roomId)) return UpsertResult::Hidden;
    int row = findRowByRoomId(room.roomId);
    if (row >= 0) {
        bool changed = false;
        auto& existing = rooms_[static_cast<size_t>(row)];
        if (existing.name != room.name && !room.name.empty()) {
            existing.name = room.name;
            changed = true;
        }
        if (existing.lastActivityTs != room.lastActivityTs) {
            existing.lastActivityTs = room.lastActivityTs;
            existing.lastMessage = room.lastMessage;
            existing.lastSender = room.lastSender;
            changed = true;
        }
        if (existing.unreadCount != room.unreadCount) {
            existing.unreadCount = room.unreadCount;
            changed = true;
        }
        if (existing.avatarUrl != room.avatarUrl && !room.avatarUrl.empty()) {
            existing.avatarUrl = room.avatarUrl;
            changed = true;
        }
        if (existing.isEncrypted != room.isEncrypted) {
            existing.isEncrypted = room.isEncrypted;
            changed = true;
        }
        if (existing.isInvite != room.isInvite) {
            existing.isInvite = room.isInvite;
            changed = true;
        }
        if (existing.inviterId != room.inviterId && !room.inviterId.empty()) {
            existing.inviterId = room.inviterId;
            changed = true;
        }
        if (existing.memberCount != room.memberCount && room.memberCount > 0) {
            existing.memberCount = room.memberCount;
            changed = true;
        }
        if (existing.stateLoaded != room.stateLoaded && room.stateLoaded)
            existing.stateLoaded = true;
        lastUpsertRow_ = row;
        return changed ? UpsertResult::Updated : UpsertResult::Unchanged;
    }

    // Insert new — sorted by lastActivity descending.
    auto it = std::upper_bound(rooms_.begin(), rooms_.end(), room,
        [](const RoomData& a, const RoomData& b) {
            return a.lastActivityTs > b.lastActivityTs;
        });
    int newRow = static_cast<int>(std::distance(rooms_.begin(), it));
    rooms_.insert(it, room);
    for (int i = newRow; i < static_cast<int>(rooms_.size()); ++i) {
        index_[rooms_[static_cast<size_t>(i)].roomId] = i;
    }
    lastUpsertRow_ = newRow;
    return UpsertResult::Inserted;
}

void RoomState::clear() {
    rooms_.clear();
    index_.clear();
}

bool RoomState::removeRoom(const std::string& roomId) {
    int row = findRowByRoomId(roomId);
    if (row < 0) return false;
    rooms_.erase(rooms_.begin() + row);
    index_.erase(roomId);
    hiddenRoomIds_.erase(roomId);
    for (int i = row; i < static_cast<int>(rooms_.size()); ++i) {
        index_[rooms_[static_cast<size_t>(i)].roomId] = i;
    }
    lastRemoveRow_ = row;
    return true;
}

int RoomState::findRowByRoomId(const std::string& roomId) const {
    auto it = index_.find(roomId);
    if (it == index_.end()) return -1;
    if (it->second < 0 || it->second >= static_cast<int>(rooms_.size()) ||
        rooms_[static_cast<size_t>(it->second)].roomId != roomId) {
        return -1;
    }
    return it->second;
}

const RoomData* RoomState::at(int row) const {
    if (row < 0 || row >= static_cast<int>(rooms_.size())) return nullptr;
    return &rooms_[static_cast<size_t>(row)];
}

int RoomState::joinedCount() const {
    int count = 0;
    for (const auto& r : rooms_) {
        if (!r.isInvite) count++;
    }
    return count;
}

} // namespace progressive::desktop

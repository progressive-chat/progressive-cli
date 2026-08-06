#pragma once

#include <string>
#include <vector>

namespace matrixcli { namespace matrix {

struct RoomSortEntry {
    std::string room_id;
    std::string display_name;
    int64_t last_ts = 0;
    int highlight_count = 0;
    int notification_count = 0;
    bool is_direct = false;
    bool is_favourite = false;
    bool is_low_priority = false;
    bool is_server_notice = false;
    bool is_marked_unread = false;
    int manual_priority = 0;
};

// 10-tier sort: favourites > DMs > highlights > unread > marked_unread >
//               server_notices > suggested > low_priority > manual > timestamp > alpha
bool roomSortCompare(const RoomSortEntry& a, const RoomSortEntry& b);
std::string roomSectionName(const RoomSortEntry& e);

}} // namespace matrixcli::matrix

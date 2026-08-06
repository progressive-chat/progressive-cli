#include "room_sort.hpp"

namespace matrixcli { namespace matrix {

bool roomSortCompare(const RoomSortEntry& a, const RoomSortEntry& b) {
    // 1. Favourites first
    if (a.is_favourite != b.is_favourite) return a.is_favourite;
    // 2. DMs before regular rooms
    if (a.is_direct != b.is_direct) return a.is_direct;
    // 3. Unread with highlights
    bool aH = a.highlight_count > 0, bH = b.highlight_count > 0;
    if (aH != bH) return aH;
    // 4. Unread without highlights
    bool aU = a.notification_count > 0, bU = b.notification_count > 0;
    if (aU != bU) return aU;
    // 5. Marked unread
    if (a.is_marked_unread != b.is_marked_unread) return a.is_marked_unread;
    // 6. Server notices below normal
    if (a.is_server_notice != b.is_server_notice) return !a.is_server_notice;
    // 7. Low priority at bottom
    if (a.is_low_priority != b.is_low_priority) return !a.is_low_priority;
    // 8. Manual priority
    if (a.manual_priority != b.manual_priority) return a.manual_priority > b.manual_priority;
    // 9. Newest activity first
    if (a.last_ts != b.last_ts) return a.last_ts > b.last_ts;
    // 10. Alphabetical
    return a.display_name < b.display_name;
}

std::string roomSectionName(const RoomSortEntry& e) {
    if (e.is_favourite) return "Favourites";
    if (e.is_direct) return "People";
    if (e.is_low_priority) return "Low Priority";
    if (e.is_server_notice) return "System Alerts";
    return "Rooms";
}

}} // namespace matrixcli::matrix

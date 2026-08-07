#ifndef PROGRESSIVE_SEARCH_ENGINE_HPP
#define PROGRESSIVE_SEARCH_ENGINE_HPP

#include <string>
#include <vector>

namespace progressive {

struct DbEvent;

// Search engine for encrypted room messages.
// Queries the local EventDatabase to find messages matching a search term.
// Works with encrypted rooms because it searches decrypted bodies stored in the local DB.
class RoomSearchEngine {
public:
    RoomSearchEngine(class EventDatabase* db) : db_(db) {}

    // Search events in a room by term. Returns JSON array of matching events.
    std::string search(const std::string& roomId, const std::string& term,
                       int limit = 20, int offset = 0) const;

private:
    EventDatabase* db_;
};

} // namespace progressive

#endif

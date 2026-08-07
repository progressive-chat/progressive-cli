#ifndef PROGRESSIVE_EVENTDB_HPP
#define PROGRESSIVE_EVENTDB_HPP

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

struct sqlite3;

namespace progressive {

struct DbEvent {
    std::string eventId;
    std::string roomId;
    std::string senderId;
    std::string senderName;
    std::string timestamp;
    std::string body;
    std::string msgType;
    std::string eventType;
    std::string relationType;
    std::string sourceEventId;
    int64_t originServerTs = 0;
    int displayIndex = 0;
    bool isEncrypted = false;
    bool sentByMe = false;
};

struct DbAnnotations {
    std::string eventId;
    int reactionCount = 0;
    std::string topReactions; // JSON array of {key, count, addedByMe}
    bool hasBeenEdited = false;
};

class EventDatabase {
public:
    EventDatabase();
    ~EventDatabase();

    // Open the database file. Returns true on success.
    bool open(const std::string& dbPath);

    // Close the database.
    void close();

    // Initialize tables (idempotent).
    bool initSchema();

    // Insert or replace an event (bulk-insert from timeline chunk).
    void insertEvent(const DbEvent& event);

    // Insert annotations for an event.
    void insertAnnotations(const DbAnnotations& annotations);

    // Get full context data for one event (replaces 3 Realm queries).
    std::string getContextJson(const std::string& eventId) const;

    // Get events for a room, paginated.
    std::vector<DbEvent> getEvents(const std::string& roomId, int limit, int offset) const;

    // Clear all data for a room.
    void clearRoom(const std::string& roomId);

    // Clear entire database.
    void clearAll();

    // Get event count.
    int count() const;

private:
    sqlite3* db_ = nullptr;
    void exec(const char* sql) const;
};

} // namespace progressive

#endif // PROGRESSIVE_EVENTDB_HPP

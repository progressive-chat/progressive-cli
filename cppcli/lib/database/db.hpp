#pragma once

#include <string>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include "../matrix/events.hpp"

struct sqlite3;

namespace matrixcli { namespace db {

using json = nlohmann::json;

struct StoredAccount {
    std::string homeserver_url;
    std::string user_id;
    std::string access_token;
    std::string device_id;
    std::string next_batch;
    bool is_logged_in() const { return !access_token.empty(); }
};

class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::string& path);
    void close();
    bool isOpen() const { return _db != nullptr; }

    // Account
    bool saveAccount(const StoredAccount& account);
    StoredAccount loadAccount();

    // Rooms
    bool upsertRoom(const std::string& room_id, const matrix::SyncRoom& room);
    bool upsertRoom(const json& room_data, const std::string& room_id);
    std::vector<json> listRooms();
    // Delete a room and all its events (used by the demo rebuild).
    bool clearRoom(const std::string& room_id);

    // Events
    bool insertEvent(const matrix::Event& event, const std::string& decrypted = "");
    std::vector<matrix::Event> getEvents(const std::string& room_id, int limit = 50,
                                         const std::string& before_event = "",
                                         const std::string& from_event = "");
    // Fetch a single event by id (for reply-chain rendering). Returns false if absent.
    bool getEventById(const std::string& event_id, matrix::Event& ev);
    int getEventCount(const std::string& room_id);
    // Persisted UI settings (the ascii client's Settings screen).
    bool setSetting(const std::string& key, const std::string& value);
    std::string getSetting(const std::string& key,
                           const std::string& def = "");
    // Rooms where the user has an open invite (an m.room.member
    // "invite" event for them, with no later join). Matches the sender
    // by localpart so both "@user" and "@user:server" forms count.
    int inviteCount(const std::string& userId);
    // The ids of the rooms where the user has an open invite.
    std::vector<std::string> invitedRoomIds(const std::string& userId);
    // Spaces: tag a room with its parent space id; rooms carry
    // "is_space"/"space" keys in listRooms().
    bool tagRoom(const std::string& room_id, const std::string& space);

    // Full-text search
    std::vector<json> search(const std::string& query, int limit = 20);

    // Notifications
    bool insertNotification(const std::string& room_id, const std::string& event_id,
                             const std::string& sender, const std::string& body, bool highlight);
    std::vector<json> getNotifications(int limit = 50, bool unread_only = true);
    int getNotificationCount(const std::string& room_id = "");
    bool markRoomRead(const std::string& room_id);
    bool markAllRead();

private:
    void migrate();
    bool exec(const std::string& sql);

    sqlite3* _db = nullptr;
};

}} // namespace matrixcli::db

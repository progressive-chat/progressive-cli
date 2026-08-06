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

    // Events
    bool insertEvent(const matrix::Event& event, const std::string& decrypted = "");
    std::vector<matrix::Event> getEvents(const std::string& room_id, int limit = 50,
                                          const std::string& before_event = "");
    int getEventCount(const std::string& room_id);

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

#include "db.hpp"
#include "../util/logger.hpp"

#include <sqlite3.h>
#include <stdexcept>

namespace matrixcli { namespace db {

Database::Database() {}

Database::~Database() { close(); }

bool Database::open(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &_db);
    if (rc != SQLITE_OK) {
        util::Logger::instance().error("Cannot open database: " + std::string(sqlite3_errmsg(_db)));
        sqlite3_close(_db);
        _db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(_db, 5000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
    migrate();
    util::Logger::instance().info("Database opened: " + path);
    return true;
}

void Database::close() {
    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

bool Database::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(_db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        util::Logger::instance().error("DB error: " + std::string(err ? err : "unknown"));
        sqlite3_free(err);
        return false;
    }
    return true;
}

void Database::migrate() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS account (
            key TEXT PRIMARY KEY,
            value TEXT
        );

        CREATE TABLE IF NOT EXISTS rooms (
            room_id TEXT PRIMARY KEY,
            name TEXT,
            topic TEXT,
            avatar_url TEXT,
            is_direct INTEGER DEFAULT 0,
            is_encrypted INTEGER DEFAULT 0,
            member_count INTEGER DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS events (
            event_id TEXT NOT NULL,
            room_id TEXT NOT NULL,
            sender TEXT,
            type TEXT,
            content TEXT,
            origin_server_ts INTEGER DEFAULT 0,
            state_key TEXT DEFAULT '',
            redacts TEXT DEFAULT '',
            decrypted_content TEXT DEFAULT '',
            relation_type TEXT DEFAULT '',
            relates_to TEXT DEFAULT '',
            PRIMARY KEY (event_id, room_id)
        );

        CREATE INDEX IF NOT EXISTS idx_events_room_ts
            ON events(room_id, origin_server_ts DESC);

        CREATE TABLE IF NOT EXISTS notifications (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            room_id TEXT NOT NULL,
            event_id TEXT NOT NULL,
            sender TEXT,
            body TEXT,
            highlight INTEGER DEFAULT 0,
            ts INTEGER DEFAULT 0,
            read INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_notif_unread ON notifications(read, ts DESC);
    )");
}

// ── Account ──

bool Database::saveAccount(const StoredAccount& account) {
    auto set = [this](const char* key, const std::string& val) {
        auto sql = "INSERT OR REPLACE INTO account(key, value) VALUES('" +
                   std::string(key) + "', ?)";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, val.c_str(), val.size(), SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    };
    set("homeserver_url", account.homeserver_url);
    set("user_id", account.user_id);
    set("access_token", account.access_token);
    set("device_id", account.device_id);
    set("next_batch", account.next_batch);
    return true;
}

StoredAccount Database::loadAccount() {
    StoredAccount acc;
    auto get = [this](const char* key) -> std::string {
        auto sql = "SELECT value FROM account WHERE key='" + std::string(key) + "'";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr);
        std::string result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* val = (const char*)sqlite3_column_text(stmt, 0);
            if (val) result = val;
        }
        sqlite3_finalize(stmt);
        return result;
    };
    acc.homeserver_url = get("homeserver_url");
    acc.user_id = get("user_id");
    acc.access_token = get("access_token");
    acc.device_id = get("device_id");
    acc.next_batch = get("next_batch");
    return acc;
}

// ── Rooms ──

bool Database::upsertRoom(const std::string& room_id, const matrix::SyncRoom& room) {
    // Extract name/topic from state events
    std::string name, topic, avatar_url;
    for (auto& ev : room.state.events) {
        if (ev.type == "m.room.name" && ev.content.contains("name")) {
            name = ev.content["name"].get<std::string>();
        } else if (ev.type == "m.room.topic" && ev.content.contains("topic")) {
            topic = ev.content["topic"].get<std::string>();
        } else if (ev.type == "m.room.avatar" && ev.content.contains("url")) {
            avatar_url = ev.content["url"].get<std::string>();
        }
    }

    json j;
    j["name"] = name;
    j["topic"] = topic;
    j["avatar_url"] = avatar_url;
    j["member_count"] = room.joined_member_count;
    return upsertRoom(j, room_id);
}

bool Database::upsertRoom(const json& room_data, const std::string& room_id) {
    sqlite3_stmt* stmt;
    const char* sql = R"(
        INSERT OR REPLACE INTO rooms(room_id, name, topic, avatar_url, member_count)
        VALUES(?, ?, ?, ?, ?)
    )";
    std::string name = room_data.value("name", "");
    std::string topic = room_data.value("topic", "");
    std::string avatar = room_data.value("avatar_url", "");
    int count = room_data.value("member_count", 0);

    sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, room_id.c_str(), room_id.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), name.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, topic.c_str(), topic.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, avatar.c_str(), avatar.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, count);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

std::vector<json> Database::listRooms() {
    std::vector<json> result;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(_db, "SELECT room_id, name, topic, avatar_url, is_direct, is_encrypted, member_count FROM rooms ORDER BY name", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json r;
        r["room_id"] = (const char*)sqlite3_column_text(stmt, 0);
        auto name = (const char*)sqlite3_column_text(stmt, 1);
        if (name) r["name"] = name;
        auto topic = (const char*)sqlite3_column_text(stmt, 2);
        if (topic) r["topic"] = topic;
        auto avatar = (const char*)sqlite3_column_text(stmt, 3);
        if (avatar) r["avatar_url"] = avatar;
        r["is_direct"] = sqlite3_column_int(stmt, 4) != 0;
        r["is_encrypted"] = sqlite3_column_int(stmt, 5) != 0;
        r["member_count"] = sqlite3_column_int(stmt, 6);
        result.push_back(r);
    }
    sqlite3_finalize(stmt);
    return result;
}

// ── Events ──

bool Database::insertEvent(const matrix::Event& event, const std::string& decrypted) {
    sqlite3_stmt* stmt;
    const char* sql = R"(
        INSERT OR REPLACE INTO events(event_id, room_id, sender, type, content,
            origin_server_ts, state_key, redacts, decrypted_content, relation_type, relates_to)
        VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";
    sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    std::string content = event.content.dump();
    sqlite3_bind_text(stmt, 1, event.event_id.c_str(), event.event_id.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, event.room_id.c_str(), event.room_id.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, event.sender.c_str(), event.sender.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, event.type.c_str(), event.type.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, content.c_str(), content.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, event.origin_server_ts);
    sqlite3_bind_text(stmt, 7, event.state_key.c_str(), event.state_key.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, event.redacts.c_str(), event.redacts.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, decrypted.c_str(), decrypted.size(), SQLITE_TRANSIENT);
    // Extract relation info from content
    std::string rel_type, rel_to;
    if (event.content.contains("m.relates_to")) {
        auto& rel = event.content["m.relates_to"];
        rel_type = rel.value("rel_type", "");
        rel_to = rel.value("event_id", "");
    }
    sqlite3_bind_text(stmt, 10, rel_type.c_str(), rel_type.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, rel_to.c_str(), rel_to.size(), SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

std::vector<matrix::Event> Database::getEvents(const std::string& room_id, int limit,
                                                const std::string& before_event) {
    std::vector<matrix::Event> result;
    std::string sql = "SELECT event_id, sender, type, content, origin_server_ts, state_key, redacts, decrypted_content FROM events WHERE room_id = ?";
    if (!before_event.empty()) {
        sql += " AND origin_server_ts < (SELECT origin_server_ts FROM events WHERE event_id = ?)";
    }
    sql += " ORDER BY origin_server_ts DESC LIMIT ?";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    sqlite3_bind_text(stmt, idx++, room_id.c_str(), room_id.size(), SQLITE_TRANSIENT);
    if (!before_event.empty()) {
        sqlite3_bind_text(stmt, idx++, before_event.c_str(), before_event.size(), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, idx++, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        matrix::Event ev;
        ev.event_id = (const char*)sqlite3_column_text(stmt, 0);
        ev.sender = (const char*)sqlite3_column_text(stmt, 1) ?: "";
        ev.type = (const char*)sqlite3_column_text(stmt, 2) ?: "";
        auto content_str = (const char*)sqlite3_column_text(stmt, 3);
        if (content_str) {
            try { ev.content = json::parse(content_str); } catch (...) {}
        }
        ev.origin_server_ts = sqlite3_column_int64(stmt, 4);
        auto sk = (const char*)sqlite3_column_text(stmt, 5);
        if (sk) ev.state_key = sk;
        auto rd = (const char*)sqlite3_column_text(stmt, 6);
        if (rd) ev.redacts = rd;
        ev.room_id = room_id;
        result.push_back(ev);
    }
    sqlite3_finalize(stmt);
    return result;
}

int Database::getEventCount(const std::string& room_id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(_db, "SELECT COUNT(*) FROM events WHERE room_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, room_id.c_str(), room_id.size(), SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

std::vector<json> Database::search(const std::string& query, int limit) {
    std::vector<json> result;
    sqlite3_stmt* stmt;
    std::string sql = "SELECT events.event_id, events.room_id, events.sender, "
                      "events.content, events.origin_server_ts, rooms.name "
                      "FROM events "
                      "LEFT JOIN rooms ON events.room_id = rooms.room_id "
                      "WHERE events.content LIKE ? "
                      "ORDER BY events.origin_server_ts DESC LIMIT ?";
    sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr);
    std::string pattern = "%" + query + "%";
    sqlite3_bind_text(stmt, 1, pattern.c_str(), pattern.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json r;
        r["event_id"] = (const char*)sqlite3_column_text(stmt, 0);
        r["room_id"] = (const char*)sqlite3_column_text(stmt, 1);
        r["sender"] = (const char*)sqlite3_column_text(stmt, 2) ?: "";
        auto ct = (const char*)sqlite3_column_text(stmt, 3);
        if (ct) {
            try { r["content"] = json::parse(ct); } catch (...) {}
        }
        r["origin_server_ts"] = sqlite3_column_int64(stmt, 4);
        auto name = (const char*)sqlite3_column_text(stmt, 5);
        if (name) r["room_name"] = name;
        result.push_back(r);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::insertNotification(const std::string& room_id, const std::string& event_id,
                                   const std::string& sender, const std::string& body, bool highlight) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(_db, "INSERT INTO notifications(room_id,event_id,sender,body,highlight,ts) VALUES(?,?,?,?,?,?)",
        -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, room_id.c_str(), room_id.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, event_id.c_str(), event_id.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, sender.c_str(), sender.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, body.c_str(), body.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, highlight ? 1 : 0);
    sqlite3_bind_int64(stmt, 6, std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

std::vector<json> Database::getNotifications(int limit, bool unread_only) {
    std::vector<json> result;
    std::string sql = "SELECT n.id, n.room_id, n.event_id, n.sender, n.body, n.highlight, n.ts, r.name "
                      "FROM notifications n LEFT JOIN rooms r ON n.room_id = r.room_id ";
    if (unread_only) sql += "WHERE n.read = 0 ";
    sql += "ORDER BY n.ts DESC LIMIT ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json n;
        n["id"] = sqlite3_column_int(stmt, 0);
        n["room_id"] = (const char*)sqlite3_column_text(stmt, 1);
        n["event_id"] = (const char*)sqlite3_column_text(stmt, 2);
        n["sender"] = (const char*)sqlite3_column_text(stmt, 3) ?: "";
        n["body"] = (const char*)sqlite3_column_text(stmt, 4) ?: "";
        n["highlight"] = sqlite3_column_int(stmt, 5) == 1;
        n["ts"] = sqlite3_column_int64(stmt, 6);
        auto rn = (const char*)sqlite3_column_text(stmt, 7);
        if (rn) n["room_name"] = rn;
        result.push_back(n);
    }
    sqlite3_finalize(stmt);
    return result;
}

int Database::getNotificationCount(const std::string& room_id) {
    std::string sql = "SELECT COUNT(*) FROM notifications WHERE read = 0";
    if (!room_id.empty()) sql += " AND room_id = ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr);
    if (!room_id.empty()) sqlite3_bind_text(stmt, 1, room_id.c_str(), room_id.size(), SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool Database::markRoomRead(const std::string& room_id) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(_db, "UPDATE notifications SET read=1 WHERE room_id=? AND read=0", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, room_id.c_str(), room_id.size(), SQLITE_TRANSIENT);
    sqlite3_step(stmt); sqlite3_finalize(stmt);
    return true;
}

bool Database::markAllRead() {
    exec("UPDATE notifications SET read=1 WHERE read=0");
    return true;
}

}} // namespace matrixcli::db

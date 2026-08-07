#include "progressive/eventdb.hpp"
#include <sqlite3.h>
#include <android/log.h>
#include <sstream>
#include <cstring>

#define LOG_TAG "EventDB"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace progressive {

// Helper: safely extract column text, returns "" for NULL
static std::string safeCol(sqlite3_stmt* stmt, int col) {
    auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? std::string(text) : std::string();
}

EventDatabase::EventDatabase() = default;

EventDatabase::~EventDatabase() {
    close();
}

bool EventDatabase::open(const std::string& dbPath) {
    if (db_) return true;
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOGE("Cannot open database: %s", sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=-8000", nullptr, nullptr, nullptr);
    LOGD("Opened database: %s", dbPath.c_str());
    return initSchema();
}

void EventDatabase::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool EventDatabase::initSchema() {
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS events (
            event_id TEXT PRIMARY KEY,
            room_id TEXT NOT NULL,
            sender_id TEXT,
            sender_name TEXT,
            timestamp TEXT,
            body TEXT,
            msg_type TEXT,
            event_type TEXT,
            relation_type TEXT,
            source_event_id TEXT,
            origin_server_ts INTEGER,
            display_index INTEGER,
            is_encrypted INTEGER DEFAULT 0,
            sent_by_me INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_events_room ON events(room_id, display_index);
        CREATE INDEX IF NOT EXISTS idx_events_source ON events(source_event_id);

        CREATE TABLE IF NOT EXISTS annotations (
            event_id TEXT PRIMARY KEY,
            reaction_count INTEGER DEFAULT 0,
            top_reactions TEXT DEFAULT '[]',
            has_been_edited INTEGER DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS room_meta (
            room_id TEXT PRIMARY KEY,
            event_count INTEGER DEFAULT 0
        );
    )SQL";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOGE("Schema creation failed: %s", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

void EventDatabase::insertEvent(const DbEvent& event) {
    const char* sql = R"SQL(
        INSERT OR REPLACE INTO events
        (event_id, room_id, sender_id, sender_name, timestamp, body,
         msg_type, event_type, relation_type, source_event_id,
         origin_server_ts, display_index, is_encrypted, sent_by_me)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, event.eventId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, event.roomId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, event.senderId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, event.senderName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, event.timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, event.body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, event.msgType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, event.eventType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, event.relationType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, event.sourceEventId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 11, event.originServerTs);
    sqlite3_bind_int(stmt, 12, event.displayIndex);
    sqlite3_bind_int(stmt, 13, event.isEncrypted ? 1 : 0);
    sqlite3_bind_int(stmt, 14, event.sentByMe ? 1 : 0);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void EventDatabase::insertAnnotations(const DbAnnotations& annotations) {
    const char* sql = R"SQL(
        INSERT OR REPLACE INTO annotations
        (event_id, reaction_count, top_reactions, has_been_edited)
        VALUES (?, ?, ?, ?)
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, annotations.eventId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, annotations.reactionCount);
    sqlite3_bind_text(stmt, 3, annotations.topReactions.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, annotations.hasBeenEdited ? 1 : 0);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string EventDatabase::getContextJson(const std::string& eventId) const {
    const char* sql = R"SQL(
        SELECT e.event_id, e.sender_id, e.sender_name, e.timestamp, e.body,
               e.msg_type, e.event_type, e.sent_by_me, e.is_encrypted,
               e.relation_type, e.source_event_id,
               a.reaction_count, a.top_reactions, a.has_been_edited
        FROM events e
        LEFT JOIN annotations a ON e.event_id = a.event_id
        WHERE e.event_id = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return R"({"cached": false})";
    }

    sqlite3_bind_text(stmt, 1, eventId.c_str(), -1, SQLITE_TRANSIENT);

    std::ostringstream json;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        json << "{";
        json << R"("cached": true,)";
        json << R"("eventId": ")" << safeCol(stmt, 0) << R"(",)";
        json << R"("senderId": ")" << safeCol(stmt, 1) << R"(",)";
        json << R"("senderName": ")" << safeCol(stmt, 2) << R"(",)";
        json << R"("timestamp": ")" << safeCol(stmt, 3) << R"(",)";
        json << R"("body": ")" << safeCol(stmt, 4) << R"(",)";
        json << R"("msgType": ")" << safeCol(stmt, 5) << R"(",)";
        json << R"("eventType": ")" << safeCol(stmt, 6) << R"(",)";
        json << R"("sentByMe": )" << (sqlite3_column_int(stmt, 7) ? "true" : "false") << ",";
        json << R"("isEncrypted": )" << (sqlite3_column_int(stmt, 8) ? "true" : "false") << ",";
        json << R"("reactionCount": )" << sqlite3_column_int(stmt, 11) << ",";
        json << R"("topReactions": )" << safeCol(stmt, 12) << ",";
        json << R"("hasBeenEdited": )" << (sqlite3_column_int(stmt, 13) ? "true" : "false");

        auto relType = safeCol(stmt, 9);
        if (!relType.empty()) {
            json << R"(,"relationType": ")" << relType << R"(")";
            json << R"(,"sourceEventId": ")" << safeCol(stmt, 10) << R"(")";
        }
        json << "}";
    } else {
        json << R"({"cached": false})";
    }

    sqlite3_finalize(stmt);
    return json.str();
}

std::vector<DbEvent> EventDatabase::getEvents(const std::string& roomId, int limit, int offset) const {
    std::vector<DbEvent> results;
    const char* sql = R"SQL(
        SELECT event_id, room_id, sender_id, sender_name, timestamp, body,
               msg_type, event_type, relation_type, source_event_id,
               origin_server_ts, display_index, is_encrypted, sent_by_me
        FROM events WHERE room_id = ? ORDER BY display_index ASC LIMIT ? OFFSET ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;

    sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DbEvent e;
        e.eventId     = safeCol(stmt, 0);
        e.roomId      = safeCol(stmt, 1);
        e.senderId    = safeCol(stmt, 2);
        e.senderName  = safeCol(stmt, 3);
        e.timestamp   = safeCol(stmt, 4);
        e.body        = safeCol(stmt, 5);
        e.msgType     = safeCol(stmt, 6);
        e.eventType   = safeCol(stmt, 7);
        e.relationType = safeCol(stmt, 8);
        e.sourceEventId = safeCol(stmt, 9);
        e.originServerTs = sqlite3_column_int64(stmt, 10);
        e.displayIndex = sqlite3_column_int(stmt, 11);
        e.isEncrypted = sqlite3_column_int(stmt, 12) != 0;
        e.sentByMe    = sqlite3_column_int(stmt, 13) != 0;
        results.push_back(std::move(e));
    }

    sqlite3_finalize(stmt);
    return results;
}

void EventDatabase::clearRoom(const std::string& roomId) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM events WHERE room_id = ?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void EventDatabase::clearAll() {
    exec("DELETE FROM events");
    exec("DELETE FROM annotations");
    exec("DELETE FROM room_meta");
}

int EventDatabase::count() const {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM events", -1, &stmt, nullptr);
    int cnt = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return cnt;
}

void EventDatabase::exec(const char* sql) const {
    sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
}

} // namespace progressive

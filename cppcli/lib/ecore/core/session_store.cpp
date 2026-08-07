// src/core/session_store.cpp

#include "session_store.hpp"
#include <simdjson.h>
#include "debug_log.hpp"

#include <sqlite3.h>
#include <cstring>
#include <sstream>

namespace progressive::desktop {

SessionStore::SessionStore() = default;

SessionStore::~SessionStore() {
    close();
}

bool SessionStore::open(const std::string& dbPath) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (db_) close();
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        // db_ may still be non-null on failure — free it.
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    // Durability recipe — same as agora-desktop.
    // WAL + synchronous=NORMAL: fsync only on checkpoint, not per commit —
    // removes eMMC write-stall jank on PineTab. Still durable to last checkpoint.
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    if (!createSchema()) {
        close();
        return false;
    }
    return true;
}

void SessionStore::close() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (db_) {
        checkpoint();
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SessionStore::createSchema() {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS account ("
        "  user_id TEXT PRIMARY KEY,"
        "  device_id TEXT,"
        "  homeserver_url TEXT NOT NULL,"
        "  access_token TEXT NOT NULL,"
        "  refresh_token TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS sync_state ("
        "  user_id TEXT PRIMARY KEY,"
        "  since_token TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS olm_account ("
        "  pickle_key TEXT PRIMARY KEY,"
        "  pickle TEXT NOT NULL,"
        "  shared INTEGER DEFAULT 0,"
        "  uploaded_key_count INTEGER DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS e2ee_data ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS hidden_rooms ("
        "  room_id TEXT PRIMARY KEY"
        ") WITHOUT ROWID;"
        "CREATE TABLE IF NOT EXISTS cross_signing ("
        "  user_id TEXT PRIMARY KEY,"
        "  data TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS verified_devices ("
        "  user_id TEXT NOT NULL,"
        "  device_id TEXT NOT NULL,"
        "  PRIMARY KEY(user_id, device_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS key_backup ("
        "  user_id TEXT PRIMARY KEY,"
        "  version TEXT NOT NULL,"
        "  recovery_key TEXT NOT NULL,"
        "  public_key TEXT NOT NULL,"
        "  algorithm TEXT NOT NULL"
        ");";
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }

    // Migration: the pre-multi-account sync_state (id=1) has no user_id —
    // drop it so the per-user table is created fresh.
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "PRAGMA table_info(sync_state);", -1, &stmt, nullptr) == SQLITE_OK) {
            bool hasUser = false;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (sqlite3_column_text(stmt, 1) &&
                    std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) == "user_id")
                    hasUser = true;
            }
            sqlite3_finalize(stmt);
            if (!hasUser) {
                sqlite3_exec(db_, "DROP TABLE IF EXISTS sync_state;", nullptr, nullptr, nullptr);
                sqlite3_exec(db_,
                    "CREATE TABLE IF NOT EXISTS sync_state ("
                    "  user_id TEXT PRIMARY KEY,"
                    "  since_token TEXT"
                    ");", nullptr, nullptr, nullptr);
            }
        }
    }

    // Add shared column to existing olm_account tables
    sqlite3_exec(db_, "ALTER TABLE olm_account ADD COLUMN shared INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);

    sqlite3_exec(db_, "ALTER TABLE olm_account ADD COLUMN uploaded_key_count INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);

    return true;
}

void SessionStore::checkpoint() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return;
    sqlite3_wal_checkpoint_v2(db_, "main", SQLITE_CHECKPOINT_TRUNCATE,
                              nullptr, nullptr);
}

bool SessionStore::saveAccount(const AccountInfo& a) {
    LOG(LogChannel::E2EE, "saveAccount: ENTER db=%p", (void*)db_);
    if (!db_) {
        LOG(LogChannel::E2EE, "saveAccount: FAIL — db_ is NULL");
        return false;
    }
    const char* sql =
        "INSERT INTO account (user_id, device_id, homeserver_url, access_token, refresh_token) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET "
        "  device_id=excluded.device_id, "
        "  homeserver_url=excluded.homeserver_url, access_token=excluded.access_token, "
        "  refresh_token=excluded.refresh_token;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, a.userId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, a.deviceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, a.homeserverUrl.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, a.accessToken.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, a.refreshToken.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    LOG(LogChannel::E2EE, "saveAccount: SQLite rc=%d device=%s", rc, a.deviceId.c_str());
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<AccountInfo> SessionStore::loadAccount() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT user_id, device_id, homeserver_url, access_token, refresh_token FROM account ORDER BY rowid DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    std::optional<AccountInfo> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        AccountInfo a;
        a.userId        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        a.deviceId      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        a.homeserverUrl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        a.accessToken   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            a.refreshToken = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        }
        result = std::move(a);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<AccountInfo> SessionStore::listAccounts() {
    std::vector<AccountInfo> result;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return result;
    const char* sql = "SELECT user_id, device_id, homeserver_url, access_token, refresh_token FROM account;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AccountInfo a;
        a.userId        = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        a.deviceId      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        a.homeserverUrl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        a.accessToken   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL)
            a.refreshToken = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result.push_back(std::move(a));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool SessionStore::clearAccount(const std::string& userId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    if (userId.empty()) return false;
    const char* sql = "DELETE FROM account WHERE user_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

bool SessionStore::saveSyncToken(const std::string& userId, const std::string& token) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql =
        "INSERT INTO sync_state (user_id, since_token) VALUES (?, ?) "
        "ON CONFLICT(user_id) DO UPDATE SET since_token=excluded.since_token;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, token.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<std::string> SessionStore::loadSyncToken(const std::string& userId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT since_token FROM sync_state WHERE user_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> result;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        result = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool SessionStore::clearSyncToken(const std::string& userId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM sync_state WHERE user_id=?;", -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

// ---- Olm account ----

bool SessionStore::saveOlmAccount(const std::string& pickle, const std::string& pickleKey,
                                   bool shared, int uploadedKeyCount) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql =
        "INSERT OR REPLACE INTO olm_account (pickle_key, pickle, shared, uploaded_key_count) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, pickleKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pickle.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, shared ? 1 : 0);
    sqlite3_bind_int(stmt, 4, uploadedKeyCount);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<OlmAccountRecord> SessionStore::loadOlmAccount(const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT pickle, pickle_key, shared, uploaded_key_count FROM olm_account WHERE pickle_key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, pickleKey.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<OlmAccountRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        OlmAccountRecord rec;
        rec.pickle = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.pickleKey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        rec.shared = sqlite3_column_int(stmt, 2) != 0;
        rec.uploadedKeyCount = sqlite3_column_int(stmt, 3);
        result = rec;
    }
    sqlite3_finalize(stmt);
    return result;
}

// ---- E2EE data store (key-value) ----

bool SessionStore::saveMegolmSessions(const std::string& data, const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    std::string key = "megolm:" + pickleKey;
    const char* sql = "INSERT INTO e2ee_data(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, data.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<std::string> SessionStore::loadMegolmSessions(const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    std::string key = "megolm:" + pickleKey;
    const char* sql = "SELECT value FROM e2ee_data WHERE key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> r;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        r = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return r;
}

bool SessionStore::savePendingKeyRequests(const std::string& data, const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    std::string key = "pending_requests:" + pickleKey;
    const char* sql = "INSERT INTO e2ee_data(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, data.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<std::string> SessionStore::loadPendingKeyRequests(const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    std::string key = "pending_requests:" + pickleKey;
    const char* sql = "SELECT value FROM e2ee_data WHERE key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> r;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        r = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return r;
}

bool SessionStore::saveOlmSessions(const std::string& data, const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    std::string key = "olm_sessions:" + pickleKey;
    const char* sql = "INSERT INTO e2ee_data(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, data.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<std::string> SessionStore::loadOlmSessions(const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    std::string key = "olm_sessions:" + pickleKey;
    const char* sql = "SELECT value FROM e2ee_data WHERE key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> r;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        r = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return r;
}

bool SessionStore::saveOutboundSessions(const std::string& data, const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    std::string key = "outbound_megolm:" + pickleKey;
    const char* sql = "INSERT INTO e2ee_data(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, data.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<std::string> SessionStore::loadOutboundSessions(const std::string& pickleKey) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    std::string key = "outbound_megolm:" + pickleKey;
    const char* sql = "SELECT value FROM e2ee_data WHERE key=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> r;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        r = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    return r;
}

bool SessionStore::saveE2eeFlag(const std::string& key, bool value) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql = "INSERT INTO e2ee_data(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, ("flag_" + key).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, (value ? "1" : "0"), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::optional<bool> SessionStore::loadE2eeFlag(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return std::nullopt;
    const char* sql = "SELECT value FROM e2ee_data WHERE key=?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    std::string fk = "flag_" + key;
    sqlite3_bind_text(stmt, 1, fk.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<bool> r;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        r = (std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) == "1");
    sqlite3_finalize(stmt);
    return r;
}

bool SessionStore::saveRoomKeyShareMarker(const std::string& userId,
    const std::string& roomId, const std::string& memberId,
    const std::string& eventId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const std::string key = "share:" + userId + ":" + roomId + ":" + memberId + ":" + eventId;
    const char* sql = "INSERT OR IGNORE INTO e2ee_data(key,value) VALUES(?1,'1');";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

bool SessionStore::hasRoomKeyShareMarker(const std::string& userId,
    const std::string& roomId, const std::string& memberId,
    const std::string& eventId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const std::string key = "share:" + userId + ":" + roomId + ":" + memberId + ":" + eventId;
    const char* sql = "SELECT 1 FROM e2ee_data WHERE key=?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

bool SessionStore::saveCrossSigningKeys(const std::string& userId,
    const std::string& json) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql = "INSERT INTO cross_signing(user_id, data) VALUES(?,?) "
                      "ON CONFLICT(user_id) DO UPDATE SET data=excluded.data;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<std::string> SessionStore::loadCrossSigningKeys(const std::string& userId) {
    std::optional<std::string> result;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return result;
    const char* sql = "SELECT data FROM cross_signing WHERE user_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        auto txt = sqlite3_column_text(stmt, 0);
        if (txt) result = std::string(reinterpret_cast<const char*>(txt));
    }
    sqlite3_finalize(stmt);
    return result;
}

std::string SessionStore::loadUserSigningPub(const std::string& userId) {
    std::string pub;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto xs = loadCrossSigningKeys(userId);
    if (!xs.has_value()) return pub;
    simdjson::dom::parser p;
    auto d = p.parse(*xs);
    if (d.error() != simdjson::SUCCESS) return pub;
    auto up = d.value()["user"]["pub"].get_string();
    if (up.error() == simdjson::SUCCESS) pub = std::string(up.value());
    return pub;
}

bool SessionStore::saveVerifiedDevice(const std::string& userId,
    const std::string& deviceId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql = "INSERT INTO verified_devices(user_id, device_id) "
                      "VALUES(?,?) ON CONFLICT DO NOTHING;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, deviceId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SessionStore::saveBackupInfo(const std::string& userId, const BackupInfo& info) {
    if (!db_) return false;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    const char* sql = "INSERT INTO key_backup(user_id, version, recovery_key, public_key, algorithm) "
                      "VALUES(?,?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET "
                      "version=excluded.version, recovery_key=excluded.recovery_key, "
                      "public_key=excluded.public_key, algorithm=excluded.algorithm;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, info.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, info.recoveryKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, info.publicKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, info.algorithm.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<BackupInfo> SessionStore::loadBackupInfo(const std::string& userId) {
    std::optional<BackupInfo> result;
    if (!db_) return result;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    const char* sql = "SELECT version, recovery_key, public_key, algorithm "
                      "FROM key_backup WHERE user_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        BackupInfo info;
        auto v = sqlite3_column_text(stmt, 0);
        auto rk = sqlite3_column_text(stmt, 1);
        auto pk = sqlite3_column_text(stmt, 2);
        auto al = sqlite3_column_text(stmt, 3);
        if (v) info.version = reinterpret_cast<const char*>(v);
        if (rk) info.recoveryKey = reinterpret_cast<const char*>(rk);
        if (pk) info.publicKey = reinterpret_cast<const char*>(pk);
        if (al) info.algorithm = reinterpret_cast<const char*>(al);
        result = std::move(info);
    }
    sqlite3_finalize(stmt);
    return result;
}

bool SessionStore::clearBackupInfo(const std::string& userId) {
    if (!db_) return false;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    const char* sql = "DELETE FROM key_backup WHERE user_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SessionStore::clearVerifiedDevices() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql = "DELETE FROM verified_devices;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool SessionStore::isDeviceVerified(const std::string& userId,
    const std::string& deviceId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql = "SELECT 1 FROM verified_devices WHERE user_id=? AND device_id=? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, userId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, deviceId.c_str(), -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

bool SessionStore::saveHiddenRoom(const std::string& roomId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql = "INSERT INTO hidden_rooms(room_id) VALUES(?) ON CONFLICT(room_id) DO NOTHING;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

bool SessionStore::removeHiddenRoom(const std::string& roomId) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return false;
    const char* sql = "DELETE FROM hidden_rooms WHERE room_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    checkpoint();
    return true;
}

std::vector<std::string> SessionStore::loadHiddenRooms() {
    std::vector<std::string> result;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!db_) return result;
    const char* sql = "SELECT room_id FROM hidden_rooms;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return result;
}

} // namespace progressive::desktop

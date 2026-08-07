// src/core/sync_engine.hpp — background /sync loop with backoff.
//
// Runs on a dedicated worker thread. Calls MatrixClient::syncFast() repeatedly
// (simdjson-based zero-copy parse, 50-200x faster than progressive_native's
// hand-rolled parser), emits signals when new events arrive or state changes.
// Exponential backoff on error. Persists the since-token after each successful sync.

#pragma once

#include "matrix_client.hpp"
#include "session_store.hpp"
#include "fast_sync.hpp"
#include "crypto/decryptor.hpp"
#include "crypto/verification.hpp"
#include "crypto/cross_sign.hpp"
#include <functional>
#include <string>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <string>
#include <thread>

namespace progressive::desktop {

enum class SyncEngineState {
    Stopped,
    InitialSync,        // first sync, no since-token
    Running,            // incremental sync loop
    Backoff,            // sleeping after error
    Paused,             // user paused
};

struct SyncEngineStats {
    int roomsJoined = 0;        // total joined rooms seen so far
    int invites = 0;
    int timelineEvents = 0;     // cumulative timeline events received
    int toDeviceEvents = 0;
    int decryptedEvents = 0;   // E2EE events successfully decrypted
    int decryptErrors = 0;     // E2EE events that failed to decrypt
    int errors = 0;             // consecutive error count
    int syncs = 0;              // total successful syncs
    std::string lastError;
    SyncEngineState state = SyncEngineState::Stopped;
};

class SyncEngine {
public:
    using SyncCallback = std::function<void(FastSyncResponse)>;
    using StateCallback = std::function<void(SyncEngineState, const SyncEngineStats&)>;
    using AuthErrorCallback = std::function<void()>;

    SyncEngine();
    ~SyncEngine();

    void setClient(std::shared_ptr<MatrixClient> c) { client_ = std::move(c); initVerificationManager(); }
    void setSessionStore(std::shared_ptr<SessionStore> s) {
        store_ = std::move(s);
        decryptor_.setVerifiedDeviceChecker([this](const std::string& userId,
            const std::string& deviceId) {
            return store_ ? store_->isDeviceVerified(userId, deviceId) : false;
        });
        initVerificationManager();
    }
    void onSync(SyncCallback cb) { syncCb_ = std::move(cb); }
    void onStateChange(StateCallback cb) { stateCb_ = std::move(cb); }
    // Called when the access token is invalid (M_UNKNOWN_TOKEN) — UI should
    // clear the saved session and show the login dialog.
    void onAuthError(AuthErrorCallback cb) { authErrCb_ = std::move(cb); }

    SessionStore* sessionStore() { return store_.get(); }

    // Access the E2EE decryptor (for setup at login time).
    Decryptor* decryptor() { return &decryptor_; }
    VerificationManager& verificationManager() { return verificationManager_; }
    void setPollTimeout(int ms) { syncTimeoutMs_ = ms; }
    void setBackupPathProvider(std::function<std::string()> provider) {
        backupPathProvider_ = std::move(provider);
    }

    // Upload device keys + one-time keys to the server.
    // Call once after init() + login. Non-blocking (spawns a thread).
    void uploadDeviceKeys(bool force = false);

    // Generate (if needed) + upload the fallback key. Called from the sync
    // loop when the server reports no unused fallback key of our type.
    void uploadFallbackKey();

    // Generate MSK/USK/SSK, publish via POST /keys/device_signing/upload
    // (spec), re-upload device_keys with the SSK signature, and persist the
    // private keys. No-op if already set up. On a UIA (401) challenge, stores
    // the session in uiaSession_ and returns false — retry with the password.
    bool setupCrossSigning();
    bool setupCrossSigningWithPassword(const std::string& password);
    // Regenerate + re-publish cross-signing keys (overwrites the identity;
    // UIA-aware — the password retry reuses setupCrossSigningWithPassword).
    bool resetCrossSigning();

    // ---- Key backup (Phase 7) ----
    // Create a backup version; returns the recovery key ("" on failure) —
    // the caller shows it ONCE.
    std::string createKeyBackupNow();
    // Upload the current backup (all exported sessions). Returns false on
    // HTTP failure.
    bool uploadKeyBackupNow();
    // Restore from a recovery key (fetch + decrypt + import). Returns the
    // number of sessions imported.
    int restoreKeyBackupNow(const std::string& recoveryKey);
    // Delete the current backup version on the server + the local registry.
    bool deleteKeyBackupNow();
    // Called by the sync loop: re-upload when new sessions arrived.
    void maybeUploadBackup();

    // ---- E2EE bootstrap (X1 phase 4: moved from the UI e2ee_init_handler) ----
    struct E2eeInitResult {
        bool e2eeOk = false;
        bool keysPublished = false;
    };
    // Load-or-create the olm account, restore persisted sessions, schedule the
    // device-keys upload. Pure core (Qt-free).
    E2eeInitResult initializeE2EE();
    // Persist the current crypto state (megolm/outbound/olm sessions).
    void persistCrypto();

    // ---- SSSS (cross-device secret sharing, Phase 7) ----
    // Encrypt the cross-signing private keys to account-data, unlockable with
    // the recovery key (m.secret_storage.v1.aes-hmac-sha2). True on success.
    bool uploadSsssSecrets(const std::string& recoveryKey);
    // Retrieve + decrypt the cross-signing secrets with the recovery key,
    // store them locally, and re-upload THIS device's keys SSK-signed.
    // Returns 0 on failure (missing secrets/wrong key), 1 on success.
    int retrieveSsssSecrets(const std::string& recoveryKey);
    std::string uiaSession() const { return uiaSession_; }

    const SyncEngineStats& stats() const { return stats_; }

    // Start the loop. If a saved since-token exists, continues incremental;
    // otherwise does an initial sync.
    void start();

    // Stop the loop (waits for the in-flight request to finish).
    void stop();

    // Pause / resume without losing the since-token.
    void pause();
    void resume();

private:
    // Wire the SAS MSK exchange fns (our master pub from the store, the other
    // party's master pub via /keys/query master_keys).
    void initVerificationManager();
    // Returns true if cross-signing is already PUBLISHED (via /keys/query master_keys).
    bool isCrossSigningPublished(const std::string& userId);
    // Persist the keys JSON for a user.
    void saveCrossSigningKeysJson(const std::string& userId, const CrossSigningKeys& keys);
    // Re-upload device_keys with the SSK signature (device_keys-only body).
    void reuploadDeviceKeys(const std::string& userId, const CrossSigningKeys& keys);
    // POST /keys/device_signing/upload. Returns 1=published, 0=UIA (session stashed), -1=failed.
    int publishCrossSigningKeys(const CrossSigningKeys& keys,
                                const std::string& userId,
                                const std::string& authJson);

    void run();
    void setState(SyncEngineState s);
    int computeBackoffMs(int consecutiveErrors) const;
    // Process to-device events from a sync response — handles m.room_key
    // (adds megolm inbound sessions) and m.room.encrypted (Olm 1:1, future).
    void processToDeviceEvents(const FastSyncResponse& resp);
    std::mutex persistMtx_;  // serializes persistCrypto (close vs periodic)
    bool otkCountSeen_ = false;  // /sync reported an OTK count at least once
    // Share-on-join: the current outbound megolm key must reach members who
    // join an encrypted room (and members whose devices changed). Element
    // does this on membership change; without it a joiner can never decrypt
    // anything sent with pre-existing sessions. The HTTP-heavy work runs on
    // the thread pool so the /sync loop is never stalled.
    void handleRoomKeyShares(const FastSyncResponse& resp);
    void doRoomKeyShares(const FastSyncResponse& resp, const AccountInfo& acct);
    std::mutex shareMtx_;  // guards sharedOnJoin_ + lastDeviceListShareMs_
    std::unordered_set<std::string> sharedOnJoin_;  // "roomId|userId|eventId" this session
    int64_t lastDeviceListShareMs_ = 0;
    void handleVerificationEvent(const std::string& type,
                                 const std::string& senderId,
                                 const std::string& contentJson);

    std::shared_ptr<MatrixClient> client_;
    std::shared_ptr<SessionStore> store_;
    SyncCallback syncCb_;
    StateCallback stateCb_;
    AuthErrorCallback authErrCb_;

    Decryptor decryptor_;
    VerificationManager verificationManager_;

    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};

    std::string sinceToken_;
    SyncEngineStats stats_;
    bool firstRun_ = false;  // true → next sync uses empty since (gets current state)
    int syncTimeoutMs_ = 3000;
    std::function<std::string()> backupPathProvider_;
    // Cooldown for fallback re-uploads: servers that never acknowledge the
    // fallback type would otherwise trigger an upload every sync.
    std::map<std::string, std::chrono::steady_clock::time_point> lastFallbackUploadAt_;
    std::map<std::string, std::chrono::steady_clock::time_point> lastFallbackPublishedAt_;
    std::map<std::string, int> fallbackBackoffSecs_;
    std::string uiaSession_;
    static constexpr std::chrono::seconds kFallbackForgetDelay{300};
};

} // namespace progressive::desktop

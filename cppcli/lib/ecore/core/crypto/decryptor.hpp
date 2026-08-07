// src/core/crypto/decryptor.hpp — Coordinates E2EE: Olm + Megolm.
//
// Provides:
//   - OlmAccount setup + device key signing for /keys/upload
//   - Inbound Olm 1:1 session management (receive m.room.encrypted to-device)
//   - Inbound Megolm session management (decrypt room timeline messages)
//   - Outbound Megolm session per encrypted room (encrypt outgoing messages)
//   - Room key sharing via Olm 1:1 (sends m.room_key to all devices)
#pragma once

#include "olm_account.hpp"
#include "megolm_store.hpp"
#include <progressive/room_encryption.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>
#include <unordered_set>
#include <functional>

namespace progressive::desktop {

struct DecryptionResult {
    bool ok = false;
    std::string plaintext;     // decrypted event JSON
    std::string error;
};

struct ReDecryptedEvent {
    std::string roomId;
    std::string eventId;
    std::string plaintext;     // full decrypted event JSON (type+content+sender)
    std::string senderId;
    int64_t originServerTs = 0;
};

// Room-key activity surfaced in the UI timeline ("X sent us the room key").
enum class RoomKeyEventKind { Received, Requested, Withheld, GaveUp };
struct RoomKeyNotification {
    std::string roomId;
    std::string sessionId;
    std::string fromUserId;    // Received: sender; Requested: request target
    RoomKeyEventKind kind = RoomKeyEventKind::Received;
    int attempt = 0;           // Requested: 1 = first request, 2+ = retry
    int64_t ts = 0;
    std::string detail;        // Withheld: reason string from the sender
};

// Per-room outbound megolm session. Created when we first send a message
// to an encrypted room. The session key is shared with all room members
// via m.room_key to-device events (Olm 1:1 encrypted).
struct OutboundMegolmSession {
    std::string sessionId;
    std::string sessionKey;     // base64 — for sharing with other devices
    void* session = nullptr;     // OlmOutboundGroupSession* (libolm)
    int messageIndex = 0;
    int messageCount = 0;        // messages sent with this session (rotation)
    int64_t startTimeMs = 0;     // session creation time (rotation)
};

// Pure backoff decision for the room-key request retry (testable without a
// server). Attempts: 1st retry after 30s, 2nd after 2min, 3rd after 10min,
// 4th after 1h, then give up.
inline bool shouldReRequestKey(int attempts, int64_t elapsedMs) {
    static const int64_t kSchedule[] = {30000, 120000, 600000, 3600000};
    if (attempts <= 0 || attempts > 4) return false;
    return elapsedMs >= kSchedule[attempts - 1];
}

class Decryptor {
public:
    Decryptor();
    ~Decryptor();

    // ---- Account lifecycle ----
    bool init(const std::string& accountPickle, const std::string& pickleKey,
              bool shared = false);
    bool init();
    bool isInitialized() const { return account_ && account_->isValid(); }

    // Save/load the olm account pickle for persistence.
    std::string saveAccountPickle(const std::string& pickleKey);

    OlmIdentityKeys identityKeys() const;
    std::string curve25519Key() const;
    std::string ed25519Key() const;

    // ---- Device key upload ----
    // Builds the /keys/upload body JSON with signed device_keys + one-time keys.
    // userId + deviceId identify whose keys these are.
    // Generates `count` one-time keys before building the body.
    std::string buildKeysUploadBody(const std::string& userId,
                                      const std::string& deviceId,
                                      int oneTimeKeyCount = 10,
                                      bool includeDeviceKeys = true,
                                      bool includeFallbackKey = false,
                                      const std::string& sskPrivB64 = "",
                                      const std::string& sskPubB64 = "",
                                      bool omitOneTimeKeys = false);

    // Builds the signed "fallback_keys" section JSON from the unpublished
    // fallback key (empty if none). Does NOT generate — generation is the
    // caller's job so failed uploads don't churn keys.
    std::string buildFallbackKeysSection(const std::string& userId,
                                         const std::string& deviceId);

    // Mark current one-time keys as published (call after /keys/upload success).
    // Tells libolm to discard used keys so next generateOneTimeKeys produces fresh ones.
    void markOneTimeKeysPublished();

    bool accountShared() const { return account_ ? account_->shared() : false; }
    void markAccountAsShared() { if (account_) account_->markAsShared(); }
    void setAccountShared(bool s) { if (account_) account_->setShared(s); }

    // ---- Inbound Megolm (room message decryption) ----
    // Decrypts a m.room.encrypted event (Megolm algorithm).
    // eventId + originServerTs are preserved for the pending queue (re-decryption).
    DecryptionResult decryptMegolmEvent(const std::string& roomId,
                                          const std::string& senderId,
                                          const std::string& contentJson,
                                          const std::string& eventId = "",
                                          int64_t originServerTs = 0);

    // Handle a to-device m.room_key event — adds the megolm inbound session.
    // Upon success, drains the pending queue and re-decrypts any saved events.
    bool handleRoomKey(const std::string& contentJson, const std::string& senderId = "");

    // Drain re-decrypted events (called from UI thread after sync).
    std::vector<ReDecryptedEvent> takeDecryptedEvents();
    // Drain room-key activity (received/requested) for the UI timeline rows.
    std::vector<RoomKeyNotification> takeRoomKeyNotifications();

    // One-shot note when an Olm session recovery fired (status-line hint).
    std::string takeLastOlmRecoveryNote();

    // Mark a sender's Olm 1:1 channel as broken (called when decrypt totally
    // fails). Enriches megolm decrypt errors so the UI can explain WHY a
    // message stays encrypted.
    void markOlmBroken(const std::string& senderKey);
    std::string enrichDecryptError(const std::string& senderKey,
                                   const std::string& baseError) const;

    // ---- Inbound Olm 1:1 (to-device decryption) ----
    // Handle a to-device m.room.encrypted event (Olm 1:1 algorithm).
    // Decrypts the ciphertext using an inbound OlmSession, then if the
    // inner plaintext is a m.room_key, calls handleRoomKey.
    // Returns the inner plaintext (for logging/processing) or empty on failure.
    std::string handleOlmEncryptedToDevice(const std::string& senderId,
                                              const std::string& contentJson);

    bool backupDirty() const { return megolm_ ? megolm_->backupDirty() : false; }
    void markBackupClean() { if (megolm_) megolm_->markBackupClean(); }
    void markBackupDirty() { if (megolm_) megolm_->markBackupDirty(); }

    // ---- Outbound Megolm (room message encryption) ----
    // Get or create an outbound megolm session for a room.
    // Returns the session ID (used in the m.room.encrypted event).
    // Caller must hold the session mutex while encrypting.
    std::string getOrCreateOutboundSession(const std::string& roomId);

    // Encrypt a plaintext message event JSON for a room.
    // Returns the content JSON for m.room.encrypted:
    //   {"algorithm":"m.megolm.v1.aes-sha2","ciphertext":"...","sender_key":"...",
    //    "device_id":"...","session_id":"..."}
    std::string encryptMessage(const std::string& roomId,
                                 const std::string& deviceId,
                                 const std::string& plaintextEventJson);

    // Get the session key for sharing (for m.room_key to-device events).
    // Returns empty if no outbound session exists for the room.
    std::string getOutboundSessionKey(const std::string& roomId);

    // Get the outbound session ID for a room.
    std::string getOutboundSessionId(const std::string& roomId);

    // Check if we have an outbound session for this room.
    bool hasOutboundSession(const std::string& roomId);

    // Drop the outbound session for a room (e.g. when room is left).
    void dropOutboundSession(const std::string& roomId);

    // Check if room key was already shared for current outbound session.
    bool roomKeyShared(const std::string& roomId) const;
    void markRoomKeyShared(const std::string& roomId);

    // ---- Room key sharing (full E2EE outbound) ----
    // Shares the outbound megolm session key with all room members' devices.
    // Steps:
    //   1. Query device keys for the given user IDs via /keys/query
    //   2. Claim one-time keys for each device via /keys/claim
    //   3. For each device: create OlmSession outbound, encrypt m.room_key
    //   4. Send m.room.encrypted to-device events via /sendToDevice
    // Returns true on success. Logs progress to stderr.
    // userIds: list of room member user IDs (excluding our own).
    // userId/deviceId: our identity (for excluding self + signing).
    bool shareRoomKey(const std::string& roomId,
                        const std::vector<std::string>& userIds,
                        const std::string& ourUserId,
                        const std::string& ourDeviceId,
                        const std::string& homeserverUrl,
                        const std::string& accessToken);

    // Access internal stores for direct manipulation (e.g. persistence).
    OlmAccountStore* account() { return account_.get(); }
    MegolmStore* megolm() { return megolm_.get(); }

    // Olm 1:1 session persistence.
    std::string pickleOlmSessions(const std::string& key);
    bool unpickleOlmSessions(const std::string& key, const std::string& data);
    size_t olmSessionCount();

    // Outbound Megolm session persistence.
    std::string pickleOutboundSessions(const std::string& key);
    bool unpickleOutboundSessions(const std::string& key, const std::string& data);

    // Set credentials for to-device HTTP calls (called once at E2EE init).
    void setCryptoContext(const std::string& ourUserId, const std::string& ourDeviceId,
                          const std::string& homeserverUrl, const std::string& accessToken);

    // Device list staleness tracking (from /sync device_lists).
    void markDevicesStale(const std::vector<std::string>& userIds);
    bool isDeviceStale(const std::string& userId);
    void clearStale(const std::string& userId);

    // Request re-sharing of a megolm room key (m.room_key_request to-device).
    // Throttled: one request per (room, session, senderKey) per run.
    // Handle an incoming m.room_key_request (another device asks us to
    // re-share a megolm room key). Sends m.forwarded_room_key on success.
    // requesterVerified: (userId, deviceId) SAS-verified. verifiedOnly: policy flag.
    // Import an m.forwarded_room_key (v1 export format) into the megolm store.
    bool handleForwardedRoomKey(const std::string& contentJson, const std::string& senderId = "");

    // Export all megolm keys (inbound + outbound) as a MegolmSessionData
    // JSON envelope (version 1). For backup / export file.
    std::string exportAllKeys();

    // Import a MegolmSessionData envelope. Returns number imported (>0 = ok).
    int importKeys(const std::string& json);

    // Import ONE raw v1 megolm export + replay pending events. Returns the
    // real session id ("" on failure).
    std::string importSingleSession(const std::string& roomId,
                                    const std::string& senderKey,
                                    const std::string& exportBase64);

    // Handle an incoming m.room_key_request (another device asks us to
    // re-share a megolm room key). Sends m.forwarded_room_key (Olm-encrypted)
    // on success. Verified-only policy checked internally via the checker.
    bool handleRoomKeyRequest(const std::string& contentJson,
                              const std::string& senderId);

    // m.room_key.withheld: match sender_key/device against pending requests
    // and surface "X withheld the room key: <reason>" in the right room.
    void noteWithheld(const std::string& senderKey, const std::string& fromDevice,
                      const std::string& reason);

    // Callback: "is (userId, deviceId) SAS-verified?" — used by the
    // verified-only key-sharing policy. Wired by SyncEngine to the store.
    using VerifiedDeviceChecker = std::function<bool(const std::string& userId,
                                                     const std::string& deviceId)>;
    void setVerifiedDeviceChecker(VerifiedDeviceChecker fn) { verifiedDeviceChecker_ = std::move(fn); }

    // Send an Olm-encrypted to-device event (m.room.encrypted wrapping) to a
    // single device: query keys -> claim OTK -> create outbound session ->
    // encrypt inner {type, content, sender/recipient envelope} -> PUT.
    // Established sessions are REUSED (one OTK per device ever) unless
    // forceFresh is set — the key-request path uses fresh pre-key messages
    // so a peer that evicted our session still creates a matching inbound one.
    bool sendOlmToDevice(const std::string& targetUserId,
                         const std::string& targetDeviceId,
                         const std::string& innerType,
                         const std::string& innerContent,
                         bool forceFresh = false);

    void setShareKeysVerifiedOnly(bool v) { shareKeysVerifiedOnly_ = v; }

    // Set the room's m.room.encryption config (rotation policy) from a
    // state-event content JSON. Rotation check happens at outbound reuse.
    void setRoomEncryptionConfig(const std::string& roomId,
                                 const std::string& stateContentJson);
    bool shareKeysVerifiedOnly() const { return shareKeysVerifiedOnly_; }

    // Re-send room-key requests that got no answer (a lost to-device message
    // would otherwise leave the event encrypted forever). Backoff schedule.
    // Call from the sync worker thread, once per sync.
    void maybeReRequestKeys();

    // Drop every piece of state scoped to the previous account. Called on
    // every init (login, account switch, restart).
    void clearPerAccountState();

    // Regenerate our identity keys and drop the whole 1:1 session layer.
    // Broken Olm chains (BAD_MESSAGE_MAC that even the m.dummy recovery
    // cannot rotate, because the peer keeps reusing its session) are healed:
    // the peer must create a fresh outbound session (pre-key) on next
    // contact. Keeps inbound megolm sessions (they do not depend on our
    // identity) so history stays decryptable.
    bool resetIdentity();
    void requestRoomKey(const std::string& roomId, const std::string& senderId,
                        const std::string& senderKey, const std::string& sessionId,
                        const std::string& senderDeviceId = "");

    // Manual "Ask for keys" (Element parity) — re-send the key request NOW
    // with a fresh request_id even if one is already pending.
    void reRequestKey(const std::string& roomId, const std::string& senderId,
                      const std::string& senderKey, const std::string& sessionId,
                      const std::string& senderDeviceId);

    // Element parity: re-send ALL outstanding key requests immediately
    // (fresh request_ids). Called after SAS verification completes.
    void resendAllPendingRequests();

    // Persistence for outstanding key requests (survive restarts; the
    // backoff schedule continues from the stored timestamps).
    std::string picklePendingKeyRequests();
    bool unpicklePendingKeyRequests(const std::string& json);

    // Force a new Olm session with a sender by sending m.dummy (to-device).
    // Creates an outbound Olm session, pickles+stores it so we can decrypt
    // the sender's reply. Called when we have no Olm session for the sender.
    void forceNewOlmSession(const std::string& senderId, const std::string& senderKey);

private:
    std::unique_ptr<OlmAccountStore> account_;
    std::unique_ptr<MegolmStore> megolm_;
    // Per-room outbound megolm sessions.
    std::unordered_map<std::string, OutboundMegolmSession> outboundSessions_;
    std::map<std::string, progressive::EncryptionConfig> roomEncryptionConfigs_;
    mutable std::mutex outboundMtx_;
    // Inbound Olm 1:1 sessions, keyed by (senderCurve25519).
    // We store them as pickled strings; created on-demand from pre-key messages.
    // DEBT: no GC on old Olm sessions — cap at ~20 per sender if growth becomes an issue
    std::unordered_map<std::string, std::vector<std::string>> olmSessions_;
    std::mutex olmMtx_;
    // Credentials for to-device HTTP calls (set once at E2EE init).
    std::string ctxUserId_, ctxDeviceId_, ctxHomeserver_, ctxToken_;
    struct KeyRequestState {
        int attempts = 0;            // requests sent so far (incl. the first)
        int64_t lastMs = 0;          // steady_clock ms of the last request
        std::string senderId;
        std::string senderDeviceId;
        std::string lastRequestId;                 // for request_cancellation
        std::vector<std::string> recipientDevices; // devices we asked
        bool gaveUpNotified = false;  // surfaced the give-up row after attempt 4
    };
    std::unordered_map<std::string, KeyRequestState> requestedKeys_;
    std::unordered_set<std::string> recentKeyRequests_;  // dedup by request_id (capped)
    bool shareKeysVerifiedOnly_ = false;  // policy: only share with SAS-verified devices
    VerifiedDeviceChecker verifiedDeviceChecker_;
    // m.dummy recovery throttle: senderKey -> last attempt (ms). Time-bounded
    // so a peer that rotates later (restart / Element reset) gets re-asked.
    std::unordered_map<std::string, int64_t> forcedOlm_;
    std::mutex requestMtx_;
    // Track which rooms have had their key shared for current outbound session
    std::unordered_map<std::string, bool> roomKeysShared_;
    std::unordered_set<std::string> staleDeviceUsers_;
    std::mutex staleMtx_;

    // Sign a canonical JSON string with Ed25519. Returns base64 signature.
    std::string signCanonicalJson(const std::string& canonicalJson);

    // Try to create an inbound OlmSession from a pre-key message.
    // Stores the session pickle for future use.
    bool createInboundOlmSession(const std::string& preKeyMessage,
                                    const std::string& senderIdentityKey);

    // Drain pending events for a room — re-decrypts each saved event.
    void processPending(const std::string& roomId,
                         const std::string& senderKey,
                         const std::string& sessionId);

    std::vector<ReDecryptedEvent> reDecryptedEvents_;
    std::mutex reDecryptedMtx_;

    std::vector<RoomKeyNotification> roomKeyNotifications_;
    std::mutex roomKeyNotifMtx_;
    void noteRoomKey(RoomKeyNotification n);

    void noteOlmRecovery(const std::string& senderId, const std::string& senderKey);
    std::string lastOlmRecoveryNote_;
    std::mutex olmRecoveryNoteMtx_;

    // senderKey -> last failure ms. Time-bounded (30 min) so the explanation
    // only appears while the channel is actually broken.
    std::unordered_map<std::string, int64_t> brokenOlmSenders_;
    mutable std::mutex brokenOlmMtx_;

    // Send a key request to a device list; "*" entries go as PLAIN to-device
    // (Olm encryption to a wildcard is impossible).
    bool sendKeyRequestToDevices(const std::string& senderId,
                                 const std::vector<std::string>& devices,
                                 const std::string& requestContent);
    // History-load flood gate (max 10 actual sends per 5s).
    int64_t lastRequestGateMs_ = 0;
    int requestGateCount_ = 0;

    // Reused outbound Olm sessions, keyed userId|deviceId. The peer keeps a
    // matching inbound session after our first (pre-key) message.
    struct OutboundOlmTarget;  // defined in decryptor.cpp (holds OlmSession)
    std::unordered_map<std::string, OutboundOlmTarget> outboundOlmSessions_;
    std::mutex outboundOlmMtx_;

    std::vector<std::string> resolveRequestRecipients(const std::string& senderId,
                                                      const std::string& senderDeviceId);
    void sendRequestCancellation(const KeyRequestState& st);
};

} // namespace progressive::desktop

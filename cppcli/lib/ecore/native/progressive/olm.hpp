#ifndef PROGRESSIVE_OLM_HPP
#define PROGRESSIVE_OLM_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace progressive {

// ---- libolm C++ Wrapper ----
// Wraps the libolm C API (https://gitlab.matrix.org/matrix-org/olm)
// for use by Progressive Chat's Kotlin layer via JNI.
//
// Replaces: org.matrix.android:olm-sdk (removed from Element Android in PR #8901)
//           org.matrix.rustcomponents:crypto-android (current Rust crypto SDK)
//
// libolm provides:
//   - OlmAccount:    identity keypair, one-time key management
//   - OlmSession:    Double Ratchet for 1:1 encrypted messages
//   - OlmInboundGroupSession:  Megolm inbound (receiving group messages)
//   - OlmOutboundGroupSession: Megolm outbound (sending group messages)
//   - OlmSAS:         Short Authentication String (device verification)

// ---- OLM Error Codes ----
enum class OlmError {
    None = 0,
    NotEnoughRandom,
    OutputBufferTooSmall,
    BadMessageVersion,
    BadMessageFormat,
    BadMessageMac,
    BadMessageKeyId,
    InvalidBase64,
    BadAccountKey,
    UnknownPickleVersion,
    Corruption,
    SessionNotFound,
    UnknownError
};

// ---- OlmAccount (Identity Keypair) ----

struct OlmAccountResult {
    bool success = false;
    OlmError error = OlmError::None;
    std::string data;     // output data (JSON, pickle, etc.)
};

class OlmAccount {
public:
    OlmAccount();
    ~OlmAccount();

    // Create a new account with random keys.
    OlmAccountResult create();

    // Load an account from a pickled (serialized) state.
    OlmAccountResult unpickle(const std::string& key, const std::string& pickle);

    // Save account state to a pickle string.
    OlmAccountResult pickle(const std::string& key);

    // Get the identity keys (Curve25519 + Ed25519) as JSON.
    OlmAccountResult identityKeys();

    // Generate one-time keys (count specifies how many).
    OlmAccountResult generateOneTimeKeys(int count);

    // Generate a new fallback key (only one previous key is retained).
    OlmAccountResult generateFallbackKey();

    // Get the unpublished fallback key as JSON ({"curve25519":{"AAAA":"b64"}}),
    // or empty data if no unpublished fallback key exists.
    OlmAccountResult unpublishedFallbackKey();

    // Forget the old fallback key — call ~5 min after publishing a new one
    // so late pre-key messages using the old key still decrypt.
    OlmAccountResult forgetOldFallbackKey();

    // Mark current one-time keys as published (they remain usable for inbound
    // session creation but won't be returned by one_time_keys again).
    OlmAccountResult markKeysAsPublished();

    // Get the maximum number of one-time keys.
    int maxOneTimeKeys();

    // Sign a message with the Ed25519 key.
    OlmAccountResult sign(const std::string& message);

    // Get the account's Ed25519 fingerprint key.
    OlmAccountResult ed25519Key();

    // Get the account's Curve25519 identity key.
    OlmAccountResult curve25519Key();

private:
    void* account_;  // OlmAccount*
    friend class OlmSession;
    friend class OlmInboundGroupSession;
    friend class OlmOutboundGroupSession;
};

// ---- OlmSession (1:1 Double Ratchet) ----

struct OlmSessionResult {
    bool success = false;
    OlmError error = OlmError::None;
    std::string data;
    int messageType = 0; // 0 = pre-key, 1 = message
};

class OlmSession {
public:
    OlmSession();
    ~OlmSession();

    // Create an outbound session (Alice side).
    OlmSessionResult createOutbound(OlmAccount& account,
        const std::string& theirIdentityKey, const std::string& theirOneTimeKey);

    // Create an inbound session from a pre-key message (Bob side).
    OlmSessionResult createInbound(OlmAccount& account, const std::string& preKeyMessage);

    // Create an inbound session from a regular message.
    OlmSessionResult createInboundFrom(OlmAccount& account,
        const std::string& theirIdentityKey, const std::string& encryptedMessage);

    // Encrypt a plaintext message.
    OlmSessionResult encrypt(const std::string& plaintext);

    // Decrypt a message.
    OlmSessionResult decrypt(const std::string& encryptedMessage, int messageType);

    // Pickle/unpickle for persistence.
    OlmSessionResult pickle(const std::string& key);
    OlmSessionResult unpickle(const std::string& key, const std::string& pickle);

    // Check if pre-key messages match this session.
    bool matchesInbound(const std::string& preKeyMessage);

    // Access the underlying ::OlmSession* for libolm error queries.
    void* rawSession() const { return session_; }

private:
    void* session_;  // OlmSession*
};

// ---- Megolm (Group Ratchet) ----

struct MegolmSessionResult {
    bool success = false;
    OlmError error = OlmError::None;
    std::string data;        // encrypted message or decrypted plaintext
    int messageIndex = 0;    // message index in the ratchet
    std::string sessionId;   // unique session identifier
    std::string sessionKey;  // key for sharing
};

struct MegolmInboundResult {
    bool success = false;
    OlmError error = OlmError::None;
    std::string plaintext;
    int messageIndex = 0;
    bool keysProved = false;      // Ed25519 key verified
};

class OlmOutboundGroupSession {
public:
    OlmOutboundGroupSession();
    ~OlmOutboundGroupSession();

    // Create a new outbound Megolm session.
    MegolmSessionResult create();

    // Encrypt a plaintext message (advances ratchet).
    MegolmSessionResult encrypt(const std::string& plaintext);

    // Get the session identifier.
    MegolmSessionResult sessionId();

    // Get the current message index.
    int messageIndex();

    // Get the session key for sharing with new participants.
    MegolmSessionResult sessionKey();

    // Pickle/unpickle.
    MegolmSessionResult pickle(const std::string& key);
    MegolmSessionResult unpickle(const std::string& key, const std::string& pickle);

private:
    void* session_;  // OlmOutboundGroupSession*
};

class OlmInboundGroupSession {
public:
    OlmInboundGroupSession();
    ~OlmInboundGroupSession();

    // Create an inbound session from a shared session key.
    MegolmInboundResult create(const std::string& sessionKey);

    // Decrypt a message.
    MegolmInboundResult decrypt(const std::string& encryptedMessage);

    // Import/export for persistence.
    MegolmSessionResult importSession(const std::string& sessionKey);
    MegolmSessionResult exportSession(const std::string& messageIndex);

    // Pickle/unpickle.
    MegolmSessionResult pickle(const std::string& key);
    MegolmSessionResult unpickle(const std::string& key, const std::string& pickle);

    // Check if this session matches.
    bool isVerified() const;

    // Get the first known index.
    int firstKnownIndex() const;

private:
    void* session_;  // OlmInboundGroupSession*
};

// ---- SAS (Short Authentication String) ----

struct SasResult {
    bool success = false;
    OlmError error = OlmError::None;
    std::string data;         // public key, MAC, or emoji codes
    bool complete = false;
};

class OlmSas {
public:
    OlmSas();
    ~OlmSas();

    // Create a new SAS object.
    SasResult create();

    // Get the public key to send to the other party.
    SasResult getPublicKey();

    // Set the other party's public key.
    SasResult setTheirPublicKey(const std::string& key);

    // Generate bytes for the SAS calculation.
    // extraInfo: "MATRIX_KEY_VERIFICATION_SAS" + sorted identities
    SasResult generateBytes(const std::string& extraInfo, int length = 6);

    // Calculate the MAC for verification.
    SasResult calculateMac(const std::string& input, const std::string& info);

    // Get the emoji codes (7 numbers 0-63) for visual comparison.
    SasResult getEmojiCodes();

    // Check if SAS verification is complete.
    bool isComplete() const;

private:
    void* sas_;  // OlmSAS*
};

// ---- Utility ----

// Generate random bytes using libolm's RNG.
std::string generateRandomBytes(int count);

// Convert OlmError to human-readable string.
std::string olmErrorToString(OlmError error);

// Format a pickled session for storage.
std::string formatPickle(const std::string& type, const std::string& pickle);

} // namespace progressive

#endif // PROGRESSIVE_OLM_HPP

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <memory>

namespace progressive {

// ==== Native Olm Account ====
//
// Wraps libolm's OlmAccount for device identity and one-time key management.
// Each device has one OlmAccount with its identity (Ed25519 + Curve25519) keys.

struct OlmAccountData {
    void* account = nullptr;       // OlmAccount*
    std::string userId;
    std::string deviceId;
    std::string identityKeysJson;  // {"ed25519":"...","curve25519":"..."}
    bool valid = false;
};

// Create a new Olm account (generates identity keys).
OlmAccountData createOlmAccount(const std::string& userId, const std::string& deviceId);

// Load account from pickled data.
OlmAccountData unpickleOlmAccount(const std::string& pickled, const std::string& userId, const std::string& deviceId);

// Pickle (serialize) account for storage.
std::string pickleOlmAccount(const OlmAccountData& account);

// Get identity keys as JSON string.
std::string getAccountIdentityKeys(const OlmAccountData& account);

// Generate one-time keys (returns JSON: {"curve25519:AAAA...":"...", ...}).
std::string generateOneTimeKeys(OlmAccountData& account, int count);

// Generate a fallback key.
std::string generateFallbackKey(OlmAccountData& account);

// Sign a message with the account's Ed25519 key.
std::string accountSign(const OlmAccountData& account, const std::string& message);

// Destroy an account.
void destroyOlmAccount(OlmAccountData& account);

// ==== Native Olm Session ====
//
// Wraps libolm's OlmSession for peer-to-peer encrypted messaging.
// Used to receive Megolm room keys and send encrypted key shares.

struct OlmSessionData {
    void* session = nullptr;       // OlmSession*
    std::string sessionId;         // Unique session identifier
    std::string theirIdentityKey;  // Curve25519 key of the other party
    bool valid = false;
    bool isInbound = true;
};

// Create an inbound session from a PRE_KEY message.
// theirIdentityKey: the Curve25519 identity key of the sender
// oneTimeKeyMessage: base64-encoded m.olm.v1.curve25519-aes-sha2 pre-key message
OlmSessionData createInboundOlmSession(OlmAccountData& account,
    const std::string& theirIdentityKey, const std::string& oneTimeKeyMessage);

// Create an outbound session for sending to another device.
// theirIdentityKey: Curve25519 identity key of the recipient
// theirOneTimeKey: Curve25519 one-time key of the recipient
OlmSessionData createOutboundOlmSession(OlmAccountData& account,
    const std::string& theirIdentityKey, const std::string& theirOneTimeKey);

// Encrypt a message using an Olm session.
// Returns base64-encoded ciphertext, or empty on failure.
std::string olmEncryptMessage(OlmSessionData& session, const std::string& plaintext);

// Decrypt a message using an Olm session.
// Returns plaintext, or empty on failure.
std::string olmDecryptMessage(OlmSessionData& session, const std::string& ciphertextBase64);

// Check if a pre-key message matches an existing inbound session.
bool olmMatchesInboundSession(OlmSessionData& session, const std::string& theirIdentityKey,
    const std::string& oneTimeKeyMessage);

// Pickle session for storage.
std::string pickleOlmSession(const OlmSessionData& session);

// Load session from pickled data.
OlmSessionData unpickleOlmSession(const std::string& pickled, OlmAccountData& account);

// Destroy a session.
void destroyOlmSession(OlmSessionData& session);

// ==== Ed25519 Signature Verification ====

// Verify an Ed25519 signature using libolm.
// key: raw Ed25519 public key bytes
// message: the signed message
// signature: raw signature bytes
// Returns true if the signature is valid.
bool ed25519Verify(const uint8_t* key, size_t keyLen,
                   const uint8_t* message, size_t messageLen,
                   const uint8_t* signature, size_t signatureLen);

// Verify a Matrix device signature.
// deviceKeysJson: the JSON device keys object
// userId: the user being verified
// deviceId: the device being verified  
// signKeyB64: base64-encoded Ed25519 signing key
// signatureB64: base64-encoded signature
bool verifyDeviceSignature(const std::string& deviceKeysJson,
                           const std::string& userId, const std::string& deviceId,
                           const std::string& signKeyB64, const std::string& signatureB64);

// ==== Device Fingerprint ====

// Compute a display-friendly device fingerprint from an identity key.
// Uses word-based encoding (like Signal/Session).
std::string computeDeviceFingerprint(const std::string& identityKeyBase64);

// ==== Olm Session Manager ====

class OlmSessionManager {
public:
    // Set the account for this device.
    void setAccount(OlmAccountData* account) { account_ = account; }

    // Add an inbound session.
    void addSession(const std::string& senderKey, const std::string& sessionId,
                    OlmSessionData session);

    // Find a matching session for a pre-key message.
    OlmSessionData* findSession(const std::string& senderKey, const std::string& sessionId);

    // Remove a session.
    void removeSession(const std::string& senderKey, const std::string& sessionId);

    // Clear all sessions.
    void clearAll();

    // Session count.
    int sessionCount() const { return (int)sessions_.size(); }

private:
    OlmAccountData* account_ = nullptr;
    struct SessionKey {
        std::string senderKey;
        std::string sessionId;
        bool operator==(const SessionKey& o) const {
            return senderKey == o.senderKey && sessionId == o.sessionId;
        }
    };
    struct SessionKeyHash {
        size_t operator()(const SessionKey& k) const {
            return std::hash<std::string>()(k.senderKey + k.sessionId);
        }
    };
    std::unordered_map<SessionKey, OlmSessionData, SessionKeyHash> sessions_;
};

// ==== Event Signing Pipeline ====

// Sign a Matrix event with the Olm account's Ed25519 key.
// Returns the event JSON with signature attached.
std::string signEvent(const OlmAccountData& account, const std::string& eventJson);

// Verify a Matrix event's Ed25519 signature.
// Returns true if the signature is valid for the given signing key.
bool verifyEventSignature(const std::string& eventJson, const std::string& signKeyB64);

} // namespace progressive

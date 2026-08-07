#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <memory>

namespace progressive {

// ==== Megolm Decryptor ====
//
// Wraps libolm's inbound_group_session for Megolm message decryption.
// Manages imported room keys and decrypts Megolm-encrypted events.
// Opt-in via Labs: SETTINGS_LABS_NATIVE_CRYPTO

struct MegolmSession {
    void* session = nullptr;      // OlmInboundGroupSession*
    std::string sessionId;        // Unique session identifier
    std::string senderKey;        // Curve25519 key of sender
    uint32_t firstKnownIndex = 0; // First known message index
    bool valid = false;
};

// Parse session key (base64-encoded Megolm session export).
// Returns false if the key is invalid.
bool parseMegolmSessionKey(const std::string& keyBase64, std::vector<uint8_t>& sessionKey);

// Create an inbound Megolm session from a session key.
// sessionKeyBase64: base64-encoded session_key (libolm decodes internally)
// Returns a valid MegolmSession or invalid on failure.
MegolmSession createInboundMegolmSession(const std::string& sessionKeyBase64);

// Destroy a Megolm session.
void destroyMegolmSession(MegolmSession& session);

// Decrypt a Megolm-encrypted message.
// session: the inbound Megolm session
// ciphertext: raw ciphertext bytes
// Returns: decrypted plaintext, or empty string on failure
std::string megolmDecrypt(MegolmSession& session, const std::string& ciphertext);

// Get the session ID (for matching events to sessions).
std::string getMegolmSessionId(const MegolmSession& session);

// Export a Megolm session (for key backup/sharing).
std::string exportMegolmSession(const MegolmSession& session);

// ==== Megolm Session Manager ====

class MegolmSessionManager {
public:
    // Add a session for a room/sender key pair.
    bool addSession(const std::string& roomId, const std::string& senderKey,
                    const std::string& sessionId, const std::string& sessionKeyBase64);

    // Add a session from a v1 EXPORT-format key (m.forwarded_room_key / backup
    // restore). Uses olm_import_inbound_group_session; stores under the REAL
    // session id read back from the imported session. Returns the real session
    // id, or empty string on failure.
    std::string addImportedSession(const std::string& roomId, const std::string& senderKey,
                                   const std::string& sessionKeyExportBase64);

    // Find a matching session for an event.
    MegolmSession* findSession(const std::string& roomId, const std::string& senderKey,
                               const std::string& sessionId);

    // Remove all sessions for a room.
    void clearRoom(const std::string& roomId);

    // Remove all sessions.
    void clearAll();

    // Export ALL inbound sessions as a MegolmSessionData JSON envelope
    // (version 1, rooms -> sessions). For key backup / export.
    std::string exportAllSessionsJson();

    // Session count.
    int sessionCount() const { return (int)sessions_.size(); }

private:
    struct SessionKey {
        std::string roomId;
        std::string senderKey;
        std::string sessionId;
        bool operator==(const SessionKey& o) const {
            return roomId == o.roomId && senderKey == o.senderKey && sessionId == o.sessionId;
        }
    };
    struct SessionKeyHash {
        size_t operator()(const SessionKey& k) const {
            return std::hash<std::string>()(k.roomId + k.senderKey + k.sessionId);
        }
    };
    std::unordered_map<SessionKey, MegolmSession, SessionKeyHash> sessions_;
};

} // namespace progressive

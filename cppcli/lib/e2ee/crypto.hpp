#pragma once

#include "olm.hpp"
#include "megolm.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <chrono>

namespace matrixcli { namespace e2ee {

struct DeviceKeys {
    std::string userId;
    std::string deviceId;
    std::string ed25519Key;
    std::string curve25519Key;
    std::string signedKeysJson;
    bool verified = false;
};

struct MegolmInboundSessionInfo {
    MegolmInboundSession session;
    std::string roomId;
    std::string senderKey;
    std::string sessionId;
    uint64_t lastReceivedTs = 0;
    uint64_t messageCount = 0;
    bool isVerified = false;
};

struct MegolmOutboundSessionInfo {
    MegolmOutboundSession session;
    std::string roomId;
    std::string sessionId;
    uint32_t messageIndex = 0;
};

class CryptoManager {
public:
    CryptoManager();
    ~CryptoManager();

    CryptoManager(const CryptoManager&) = delete;
    CryptoManager& operator=(const CryptoManager&) = delete;

    // Account management
    void initAccount(const std::string& userId, const std::string& deviceId);
    void loadAccount(const std::string& userId, const std::string& deviceId,
                     const std::string& pickled, const std::string& pickleKey);
    std::string pickleAccount(const std::string& pickleKey);
    DeviceKeys deviceKeys() const;
    std::string signMessage(const std::string& message);

    // One-time keys
    std::string generateOneTimeKeys(size_t count);
    std::string getOneTimeKeys();
    void markKeysPublished();
    std::string generateFallbackKey();
    std::string getFallbackKey();
    bool needsMoreOneTimeKeys(size_t threshold = 10) const;

    // Olm sessions (per-device peer-to-peer)
    void createOutboundOlmSession(const std::string& theirDeviceKey,
                                   const std::string& theirOneTimeKey);
    void createInboundOlmSession(const std::string& theirDeviceKey,
                                  const std::string& preKeyMessage);
    std::string encryptOlm(const std::string& theirDeviceKey,
                            const std::string& plaintext);
    std::string decryptOlm(const std::string& theirDeviceKey,
                            const std::string& ciphertext);
    // Try to find a matching session for a pre-key message
    bool hasOlmSession(const std::string& theirDeviceKey);

    // Megolm sessions (per-room group messaging)
    std::string startMegolmOutbound(const std::string& roomId);
    std::string encryptMegolm(const std::string& roomId,
                               const std::string& plaintext,
                               std::string& outSessionId,
                               std::string& outCiphertext);
    void receiveMegolmSession(const std::string& roomId,
                               const std::string& senderKey,
                               const std::string& sessionKey);
    std::string decryptMegolm(const std::string& roomId,
                               const std::string& sessionId,
                               const std::string& ciphertext,
                               uint32_t& messageIndex);

    // Megolm session key export/import
    std::string exportMegolmSessionKey(const std::string& roomId,
                                        const std::string& sessionId,
                                        uint32_t messageIndex);
    void importMegolmSessionKey(const std::string& roomId,
                                 const std::string& senderKey,
                                 const std::string& sessionKey);

    // Store management
    void clearRoom(const std::string& roomId);
    void clearAll();

    bool accountValid() const { return _accountValid; }

private:
    OlmAccount _account;
    bool _accountValid = false;
    std::string _userId;
    std::string _deviceId;

    // Olm sessions indexed by device curve25519 key
    std::map<std::string, OlmSession> _olmSessions;
    // Track which olm sessions have sent the pre-key message
    std::map<std::string, bool> _olmPrekeySent;

    // Megolm inbound sessions for decryption, indexed by sessionId
    std::map<std::string, MegolmInboundSessionInfo> _inboundMegolm;
    // Track per-room active outbound session
    std::map<std::string, MegolmOutboundSessionInfo> _outboundMegolm;

    static uint64_t _nowMs();
};

// Base64 helpers exposed for use by other e2ee modules
std::string b64Encode(const uint8_t* data, size_t len);
std::string b64Encode(const std::string& data);
std::vector<uint8_t> b64Decode(const std::string& input);

}} // namespace matrixcli::e2ee

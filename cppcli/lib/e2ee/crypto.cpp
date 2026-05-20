#include "crypto.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <cstring>
#include <chrono>

namespace matrixcli { namespace e2ee {

using json = nlohmann::json;

static const char b64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64Encode(const uint8_t* data, size_t len) {
    std::string out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64Table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        out.push_back(b64Table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

std::string b64Encode(const std::string& data) {
    return b64Encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::vector<uint8_t> b64Decode(const std::string& input) {
    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (char c : input) {
        if (c == '=') break;
        const char* p = strchr(b64Table, c);
        if (!p) continue;
        val = (val << 6) + static_cast<int>(p - b64Table);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

uint64_t CryptoManager::_nowMs() {
    auto now = std::chrono::system_clock::now();
    auto dur = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
}

// ============================================================
// CryptoManager
// ============================================================

CryptoManager::CryptoManager() = default;

CryptoManager::~CryptoManager() = default;

void CryptoManager::initAccount(const std::string& userId,
                                  const std::string& deviceId) {
    _userId = userId;
    _deviceId = deviceId;
    _account.create();
    _accountValid = true;
}

void CryptoManager::loadAccount(const std::string& userId,
                                  const std::string& deviceId,
                                  const std::string& pickled,
                                  const std::string& pickleKey) {
    _userId = userId;
    _deviceId = deviceId;
    _account = OlmAccount::unpickle(pickleKey, pickled);
    _accountValid = true;
}

std::string CryptoManager::pickleAccount(const std::string& pickleKey) {
    return _account.pickle(pickleKey);
}

DeviceKeys CryptoManager::deviceKeys() const {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");

    DeviceKeys keys;
    keys.userId = _userId;
    keys.deviceId = _deviceId;

    std::string identityJson = _account.identityKeys();
    auto j = json::parse(identityJson);
    keys.ed25519Key = j.value("ed25519", "");
    keys.curve25519Key = j.value("curve25519", "");
    keys.signedKeysJson = identityJson;

    return keys;
}

std::string CryptoManager::signMessage(const std::string& message) {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");
    return _account.sign(message);
}

std::string CryptoManager::generateOneTimeKeys(size_t count) {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");
    _account.generateOneTimeKeys(count);
    return _account.oneTimeKeys();
}

std::string CryptoManager::getOneTimeKeys() {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");
    return _account.oneTimeKeys();
}

void CryptoManager::markKeysPublished() {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");
    _account.markKeysPublished();
}

std::string CryptoManager::generateFallbackKey() {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");
    _account.generateFallbackKey();
    return _account.unpublishedFallbackKey();
}

std::string CryptoManager::getFallbackKey() {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");
    return _account.unpublishedFallbackKey();
}

bool CryptoManager::needsMoreOneTimeKeys(size_t threshold) const {
    if (!_accountValid) return true;
    try {
        std::string keysJson = const_cast<OlmAccount&>(_account).oneTimeKeys();
        auto j = json::parse(keysJson);
        size_t count = 0;
        if (j.contains("curve25519"))
            count = j["curve25519"].size();
        return count < threshold;
    } catch (...) {
        return true;
    }
}

// ============================================================
// Olm session management
// ============================================================

void CryptoManager::createOutboundOlmSession(
    const std::string& theirDeviceKey,
    const std::string& theirOneTimeKey) {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");

    OlmSession session;
    session.createOutbound(_account, theirDeviceKey, theirOneTimeKey);
    _olmSessions[theirDeviceKey] = std::move(session);
    _olmPrekeySent[theirDeviceKey] = false;
}

void CryptoManager::createInboundOlmSession(
    const std::string& theirDeviceKey,
    const std::string& preKeyMessage) {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");

    auto it = _olmSessions.find(theirDeviceKey);
    if (it != _olmSessions.end() && it->second.matchesInbound(preKeyMessage)) {
        // Existing session matched; the pre-key message might be a
        // duplicate, but the session is already established.
        return;
    }

    OlmSession session;
    session.createInboundFrom(_account, theirDeviceKey, preKeyMessage);
    _olmSessions[theirDeviceKey] = std::move(session);
}

std::string CryptoManager::encryptOlm(const std::string& theirDeviceKey,
                                       const std::string& plaintext) {
    auto it = _olmSessions.find(theirDeviceKey);
    if (it == _olmSessions.end())
        throw std::runtime_error("CryptoManager: no Olm session for device " +
                                 theirDeviceKey);

    size_t msgType = it->second.encryptMessageType();
    std::string ciphertext = it->second.encrypt(plaintext);

    // Build Matrix m.room.encrypted event content for Olm
    json content;
    content["algorithm"] = "m.olm.v1.curve25519-aes-sha2";

    auto dk = deviceKeys();
    content["sender_key"] = dk.curve25519Key;

    json ciphertextObj;
    ciphertextObj[theirDeviceKey] = {
        {"type", msgType},
        {"body", ciphertext}
    };
    content["ciphertext"] = ciphertextObj;

    if (msgType == 0) // PRE_KEY - remove one-time keys from account
        it->second.removeOneTimeKeys(_account);

    // Mark that we've sent the pre-key
    _olmPrekeySent[theirDeviceKey] = true;

    return content.dump();
}

std::string CryptoManager::decryptOlm(const std::string& theirDeviceKey,
                                       const std::string& ciphertext) {
    auto it = _olmSessions.find(theirDeviceKey);
    if (it == _olmSessions.end())
        throw std::runtime_error("CryptoManager: no Olm session for device " +
                                 theirDeviceKey);

    // The ciphertext from the Matrix event: try message type 1 first
    size_t msgType = 1;
    try {
        return it->second.decrypt(msgType, ciphertext);
    } catch (const std::runtime_error&) {
        // Try pre-key type
        msgType = 0;
        try {
            return it->second.decrypt(msgType, ciphertext);
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(
                std::string("olm decrypt failed for both types: ") + e.what());
        }
    }
}

bool CryptoManager::hasOlmSession(const std::string& theirDeviceKey) {
    return _olmSessions.find(theirDeviceKey) != _olmSessions.end();
}

// ============================================================
// Megolm session management
// ============================================================

std::string CryptoManager::startMegolmOutbound(const std::string& roomId) {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");

    MegolmOutboundSession outSession;
    outSession.init();

    std::string sesId = outSession.sessionId();

    MegolmOutboundSessionInfo info;
    info.session = std::move(outSession);
    info.roomId = roomId;
    info.sessionId = sesId;
    info.messageIndex = 0;
    _outboundMegolm[roomId] = std::move(info);

    return sesId;
}

std::string CryptoManager::encryptMegolm(const std::string& roomId,
                                          const std::string& plaintext,
                                          std::string& outSessionId,
                                          std::string& outCiphertext) {
    if (!_accountValid)
        throw std::runtime_error("CryptoManager: account not initialised");

    auto it = _outboundMegolm.find(roomId);
    if (it == _outboundMegolm.end()) {
        // Start a new outbound session
        startMegolmOutbound(roomId);
        it = _outboundMegolm.find(roomId);
    }

    auto& info = it->second;
    info.messageIndex = info.session.messageIndex();

    outCiphertext = info.session.encrypt(plaintext);
    outSessionId = info.sessionId;

    // Rotate session after ~100 messages
    if (info.session.messageIndex() % 100 == 0 && info.session.messageIndex() > 0) {
        auto sesKey = info.session.sessionKey();
        // Re-import as new inbound for ourselves
        try {
            MegolmInboundSession selfInbound;
            selfInbound.importSession(sesKey);
            MegolmInboundSessionInfo inInfo;
            inInfo.session = std::move(selfInbound);
            inInfo.roomId = roomId;
            inInfo.senderKey = deviceKeys().curve25519Key;
            inInfo.sessionId = sesKey;
            inInfo.lastReceivedTs = _nowMs();
            _inboundMegolm[sesKey] = std::move(inInfo);
        } catch (...) {}

        startMegolmOutbound(roomId);
    }

    return roomId;
}

void CryptoManager::receiveMegolmSession(const std::string& roomId,
                                          const std::string& senderKey,
                                          const std::string& sessionKey) {
    MegolmInboundSession session;
    session.importSession(sessionKey);

    std::string sesId = session.sessionId();

    MegolmInboundSessionInfo info;
    info.session = std::move(session);
    info.roomId = roomId;
    info.senderKey = senderKey;
    info.sessionId = sesId;
    info.lastReceivedTs = _nowMs();
    info.messageCount = 0;
    info.isVerified = false;

    _inboundMegolm[sesId] = std::move(info);
}

std::string CryptoManager::decryptMegolm(const std::string& roomId,
                                          const std::string& sessionId,
                                          const std::string& ciphertext,
                                          uint32_t& messageIndex) {
    if (sessionId.empty()) {
        // Find best inbound session for the room
        MegolmInboundSessionInfo* best = nullptr;
        for (auto& [sid, info] : _inboundMegolm) {
            if (info.roomId != roomId) continue;
            if (!best || info.lastReceivedTs > best->lastReceivedTs)
                best = &info;
        }
        if (!best)
            throw std::runtime_error(
                "CryptoManager: no inbound Megolm session for room " + roomId);
        best->lastReceivedTs = _nowMs();
        best->messageCount++;
        best->isVerified = best->session.isVerified();
        return best->session.decrypt(ciphertext, messageIndex);
    }

    auto it = _inboundMegolm.find(sessionId);
    if (it == _inboundMegolm.end())
        throw std::runtime_error(
            "CryptoManager: no inbound Megolm session " + sessionId);

    it->second.lastReceivedTs = _nowMs();
    it->second.messageCount++;
    it->second.isVerified = it->second.session.isVerified();
    return it->second.session.decrypt(ciphertext, messageIndex);
}

std::string CryptoManager::exportMegolmSessionKey(const std::string& roomId,
                                                   const std::string& sessionId,
                                                   uint32_t messageIndex) {
    auto it = _inboundMegolm.find(sessionId);
    if (it == _inboundMegolm.end())
        throw std::runtime_error(
            "CryptoManager: no inbound Megolm session " + sessionId);
    return it->second.session.exportSession(messageIndex);
}

void CryptoManager::importMegolmSessionKey(const std::string& roomId,
                                            const std::string& senderKey,
                                            const std::string& sessionKey) {
    receiveMegolmSession(roomId, senderKey, sessionKey);
}

void CryptoManager::clearRoom(const std::string& roomId) {
    auto ib = _inboundMegolm.begin();
    while (ib != _inboundMegolm.end()) {
        if (ib->second.roomId == roomId)
            ib = _inboundMegolm.erase(ib);
        else
            ++ib;
    }
    _outboundMegolm.erase(roomId);
}

void CryptoManager::clearAll() {
    _olmSessions.clear();
    _olmPrekeySent.clear();
    _inboundMegolm.clear();
    _outboundMegolm.clear();
}

}} // namespace matrixcli::e2ee

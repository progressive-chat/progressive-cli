#include "megolm.hpp"
#include <olm/olm.h>
#include <olm/inbound_group_session.h>
#include <olm/outbound_group_session.h>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <random>

namespace matrixcli { namespace e2ee {

static const char b64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
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

static std::vector<uint8_t> base64Decode(const std::string& input) {
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

static std::vector<uint8_t> randomBytes(size_t len) {
    std::vector<uint8_t> buf(len);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.is_open()) {
        urandom.read(reinterpret_cast<char*>(buf.data()), len);
        if (urandom.gcount() == static_cast<std::streamsize>(len))
            return buf;
    }
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < len; ++i)
        buf[i] = static_cast<uint8_t>(dist(rd));
    return buf;
}

using C_OutboundGroupSession = ::OlmOutboundGroupSession;
using C_InboundGroupSession = ::OlmInboundGroupSession;

// ============================================================
// MegolmOutboundSession
// ============================================================

MegolmOutboundSession::MegolmOutboundSession() {
    _buffer.resize(olm_outbound_group_session_size());
    _raw = olm_outbound_group_session(_buffer.data());
    if (!_raw)
        throw std::runtime_error("olm_outbound_group_session: failed to initialise");
}

MegolmOutboundSession::~MegolmOutboundSession() {
    if (_raw)
        olm_clear_outbound_group_session(
            static_cast<C_OutboundGroupSession*>(_raw));
}

MegolmOutboundSession::MegolmOutboundSession(MegolmOutboundSession&& other) noexcept
    : _buffer(std::move(other._buffer)), _raw(other._raw) {
    other._raw = nullptr;
}

MegolmOutboundSession& MegolmOutboundSession::operator=(
    MegolmOutboundSession&& other) noexcept {
    if (this != &other) {
        if (_raw)
            olm_clear_outbound_group_session(
                static_cast<C_OutboundGroupSession*>(_raw));
        _buffer = std::move(other._buffer);
        _raw = other._raw;
        other._raw = nullptr;
    }
    return *this;
}

void MegolmOutboundSession::init() {
    auto* s = static_cast<C_OutboundGroupSession*>(_raw);
    size_t randLen = olm_init_outbound_group_session_random_length(s);
    auto random = randomBytes(randLen);
    size_t ret = olm_init_outbound_group_session(s, random.data(), randLen);
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_init_outbound_group_session: ") +
            olm_outbound_group_session_last_error(s));
    }
}

std::string MegolmOutboundSession::encrypt(const std::string& plaintext) {
    auto* s = static_cast<C_OutboundGroupSession*>(_raw);
    size_t msgLen = olm_group_encrypt_message_length(s, plaintext.size());
    std::vector<uint8_t> msgBuf(msgLen);
    size_t ret = olm_group_encrypt(s,
        reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size(),
        msgBuf.data(), msgLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_group_encrypt: ") +
                                 olm_outbound_group_session_last_error(s));
    }
    msgBuf.resize(ret);
    return base64Encode(msgBuf.data(), msgBuf.size());
}

std::string MegolmOutboundSession::sessionId() {
    auto* s = static_cast<C_OutboundGroupSession*>(_raw);
    size_t len = olm_outbound_group_session_id_length(s);
    std::string id(len, '\0');
    size_t ret = olm_outbound_group_session_id(s,
        reinterpret_cast<uint8_t*>(&id[0]), len);
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_outbound_group_session_id: ") +
            olm_outbound_group_session_last_error(s));
    }
    id.resize(ret);
    return id;
}

uint32_t MegolmOutboundSession::messageIndex() const {
    return olm_outbound_group_session_message_index(
        const_cast<C_OutboundGroupSession*>(
            static_cast<const C_OutboundGroupSession*>(_raw)));
}

std::string MegolmOutboundSession::sessionKey() {
    auto* s = static_cast<C_OutboundGroupSession*>(_raw);
    size_t len = olm_outbound_group_session_key_length(s);
    std::string key(len, '\0');
    size_t ret = olm_outbound_group_session_key(s,
        reinterpret_cast<uint8_t*>(&key[0]), len);
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_outbound_group_session_key: ") +
            olm_outbound_group_session_last_error(s));
    }
    key.resize(ret);
    return key;
}

std::string MegolmOutboundSession::pickle(const std::string& key) {
    auto* s = static_cast<C_OutboundGroupSession*>(_raw);
    size_t len = olm_pickle_outbound_group_session_length(s);
    std::string pickled(len, '\0');
    size_t ret = olm_pickle_outbound_group_session(s,
        key.data(), key.size(), &pickled[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_pickle_outbound_group_session: ") +
            olm_outbound_group_session_last_error(s));
    }
    pickled.resize(ret);
    return pickled;
}

MegolmOutboundSession MegolmOutboundSession::unpickle(
    const std::string& key, const std::string& pickle) {
    MegolmOutboundSession session;
    auto* s = static_cast<C_OutboundGroupSession*>(session._raw);
    size_t ret = olm_unpickle_outbound_group_session(s,
        key.data(), key.size(),
        const_cast<char*>(pickle.data()), pickle.size());
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_unpickle_outbound_group_session: ") +
            olm_outbound_group_session_last_error(s));
    }
    return session;
}

// ============================================================
// MegolmInboundSession
// ============================================================

MegolmInboundSession::MegolmInboundSession() {
    _buffer.resize(olm_inbound_group_session_size());
    _raw = olm_inbound_group_session(_buffer.data());
    if (!_raw)
        throw std::runtime_error("olm_inbound_group_session: failed to initialise");
}

MegolmInboundSession::~MegolmInboundSession() {
    if (_raw)
        olm_clear_inbound_group_session(
            static_cast<C_InboundGroupSession*>(_raw));
}

MegolmInboundSession::MegolmInboundSession(MegolmInboundSession&& other) noexcept
    : _buffer(std::move(other._buffer)), _raw(other._raw) {
    other._raw = nullptr;
}

MegolmInboundSession& MegolmInboundSession::operator=(
    MegolmInboundSession&& other) noexcept {
    if (this != &other) {
        if (_raw)
            olm_clear_inbound_group_session(
                static_cast<C_InboundGroupSession*>(_raw));
        _buffer = std::move(other._buffer);
        _raw = other._raw;
        other._raw = nullptr;
    }
    return *this;
}

void MegolmInboundSession::init(const std::string& sessionKey) {
    auto key = base64Decode(sessionKey);
    auto* s = static_cast<C_InboundGroupSession*>(_raw);
    size_t ret = olm_init_inbound_group_session(s, key.data(), key.size());
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_init_inbound_group_session: ") +
            olm_inbound_group_session_last_error(s));
    }
}

void MegolmInboundSession::importSession(const std::string& sessionKey) {
    auto key = base64Decode(sessionKey);
    auto* s = static_cast<C_InboundGroupSession*>(_raw);
    size_t ret = olm_import_inbound_group_session(s, key.data(), key.size());
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_import_inbound_group_session: ") +
            olm_inbound_group_session_last_error(s));
    }
}

std::string MegolmInboundSession::decrypt(const std::string& ciphertext,
                                           uint32_t& messageIndex) {
    auto ct = base64Decode(ciphertext);
    auto* s = static_cast<C_InboundGroupSession*>(_raw);

    size_t maxLen = olm_group_decrypt_max_plaintext_length(
        s, ct.data(), ct.size());
    if (maxLen == olm_error()) {
        throw std::runtime_error(
            std::string("olm_group_decrypt_max_plaintext_length: ") +
            olm_inbound_group_session_last_error(s));
    }

    std::vector<uint8_t> plaintext(maxLen);
    size_t ret = olm_group_decrypt(s,
        ct.data(), ct.size(), plaintext.data(), maxLen, &messageIndex);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_group_decrypt: ") +
                                 olm_inbound_group_session_last_error(s));
    }
    return std::string(plaintext.begin(), plaintext.begin() + ret);
}

std::string MegolmInboundSession::sessionId() {
    auto* s = static_cast<C_InboundGroupSession*>(_raw);
    size_t len = olm_inbound_group_session_id_length(s);
    std::string id(len, '\0');
    size_t ret = olm_inbound_group_session_id(s,
        reinterpret_cast<uint8_t*>(&id[0]), len);
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_inbound_group_session_id: ") +
            olm_inbound_group_session_last_error(s));
    }
    id.resize(ret);
    return id;
}

uint32_t MegolmInboundSession::firstKnownIndex() const {
    return olm_inbound_group_session_first_known_index(
        static_cast<const C_InboundGroupSession*>(_raw));
}

bool MegolmInboundSession::isVerified() const {
    return olm_inbound_group_session_is_verified(
        static_cast<const C_InboundGroupSession*>(_raw)) != 0;
}

std::string MegolmInboundSession::exportSession(uint32_t messageIndex) {
    auto* s = static_cast<C_InboundGroupSession*>(_raw);
    size_t len = olm_export_inbound_group_session_length(s);
    if (len == olm_error()) {
        throw std::runtime_error(
            std::string("olm_export_inbound_group_session_length: ") +
            olm_inbound_group_session_last_error(s));
    }
    std::string key(len, '\0');
    size_t ret = olm_export_inbound_group_session(s,
        reinterpret_cast<uint8_t*>(&key[0]), len, messageIndex);
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_export_inbound_group_session: ") +
            olm_inbound_group_session_last_error(s));
    }
    key.resize(ret);
    return key;
}

std::string MegolmInboundSession::pickle(const std::string& key) {
    auto* s = static_cast<C_InboundGroupSession*>(_raw);
    size_t len = olm_pickle_inbound_group_session_length(s);
    std::string pickled(len, '\0');
    size_t ret = olm_pickle_inbound_group_session(s,
        key.data(), key.size(), &pickled[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_pickle_inbound_group_session: ") +
            olm_inbound_group_session_last_error(s));
    }
    pickled.resize(ret);
    return pickled;
}

MegolmInboundSession MegolmInboundSession::unpickle(
    const std::string& key, const std::string& pickle) {
    MegolmInboundSession session;
    auto* s = static_cast<C_InboundGroupSession*>(session._raw);
    size_t ret = olm_unpickle_inbound_group_session(s,
        key.data(), key.size(),
        const_cast<char*>(pickle.data()), pickle.size());
    if (ret == olm_error()) {
        throw std::runtime_error(
            std::string("olm_unpickle_inbound_group_session: ") +
            olm_inbound_group_session_last_error(s));
    }
    return session;
}

}} // namespace matrixcli::e2ee

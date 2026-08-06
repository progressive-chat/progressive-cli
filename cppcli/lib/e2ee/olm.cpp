#include "olm.hpp"
#include <olm/olm.h>
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

using C_Account = ::OlmAccount;
using C_Session = ::OlmSession;

// ============================================================
// OlmAccount
// ============================================================

OlmAccount::OlmAccount() {
    _buffer.resize(olm_account_size());
    _raw = olm_account(_buffer.data());
    if (!_raw)
        throw std::runtime_error("olm_account: failed to initialise");
}

OlmAccount::~OlmAccount() {
    if (_raw)
        olm_clear_account(static_cast<C_Account*>(_raw));
}

OlmAccount::OlmAccount(OlmAccount&& other) noexcept
    : _buffer(std::move(other._buffer)), _raw(other._raw) {
    other._raw = nullptr;
}

OlmAccount& OlmAccount::operator=(OlmAccount&& other) noexcept {
    if (this != &other) {
        if (_raw)
            olm_clear_account(static_cast<C_Account*>(_raw));
        _buffer = std::move(other._buffer);
        _raw = other._raw;
        other._raw = nullptr;
    }
    return *this;
}

void OlmAccount::create() {
    auto* r = static_cast<C_Account*>(_raw);
    size_t randLen = olm_create_account_random_length(r);
    auto random = randomBytes(randLen);
    size_t ret = olm_create_account(r, random.data(), randLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_create_account: ") +
                                 olm_account_last_error(r));
    }
}

std::string OlmAccount::identityKeys() const {
    auto* r = static_cast<C_Account*>(_raw);
    size_t len = olm_account_identity_keys_length(r);
    std::string keys(len, '\0');
    size_t ret = olm_account_identity_keys(r, &keys[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_account_identity_keys: ") +
                                 olm_account_last_error(r));
    }
    keys.resize(ret);
    return keys;
}

std::string OlmAccount::sign(const std::string& message) {
    auto* r = static_cast<C_Account*>(_raw);
    size_t sigLen = olm_account_signature_length(r);
    std::string sig(sigLen, '\0');
    size_t ret = olm_account_sign(r,
        reinterpret_cast<const uint8_t*>(message.data()), message.size(),
        &sig[0], sigLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_account_sign: ") +
                                 olm_account_last_error(r));
    }
    sig.resize(ret);
    return sig;
}

void OlmAccount::generateOneTimeKeys(size_t count) {
    auto* r = static_cast<C_Account*>(_raw);
    size_t randLen = olm_account_generate_one_time_keys_random_length(r, count);
    auto random = randomBytes(randLen);
    size_t ret = olm_account_generate_one_time_keys(r, count, random.data(), randLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_account_generate_one_time_keys: ") +
                                 olm_account_last_error(r));
    }
}

std::string OlmAccount::oneTimeKeys() {
    auto* r = static_cast<C_Account*>(_raw);
    size_t len = olm_account_one_time_keys_length(r);
    std::string keys(len, '\0');
    size_t ret = olm_account_one_time_keys(r, &keys[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_account_one_time_keys: ") +
                                 olm_account_last_error(r));
    }
    keys.resize(ret);
    return keys;
}

void OlmAccount::markKeysPublished() {
    olm_account_mark_keys_as_published(static_cast<C_Account*>(_raw));
}

size_t OlmAccount::maxOneTimeKeys() const {
    return olm_account_max_number_of_one_time_keys(
        static_cast<const C_Account*>(_raw));
}

void OlmAccount::generateFallbackKey() {
    auto* r = static_cast<C_Account*>(_raw);
    size_t randLen = olm_account_generate_fallback_key_random_length(r);
    auto random = randomBytes(randLen);
    size_t ret = olm_account_generate_fallback_key(r, random.data(), randLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_account_generate_fallback_key: ") +
                                 olm_account_last_error(r));
    }
}

std::string OlmAccount::fallbackKey() const {
    auto* r = static_cast<C_Account*>(_raw);
    size_t len = olm_account_fallback_key_length(r);
    std::string key(len, '\0');
    size_t ret = olm_account_fallback_key(r, &key[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_account_fallback_key: ") +
                                 olm_account_last_error(r));
    }
    key.resize(ret);
    return key;
}

std::string OlmAccount::unpublishedFallbackKey() const {
    auto* r = static_cast<C_Account*>(_raw);
    size_t len = olm_account_unpublished_fallback_key_length(r);
    std::string key(len, '\0');
    size_t ret = olm_account_unpublished_fallback_key(r, &key[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_account_unpublished_fallback_key: ") +
                                 olm_account_last_error(r));
    }
    key.resize(ret);
    return key;
}

void OlmAccount::forgetOldFallbackKey() {
    olm_account_forget_old_fallback_key(static_cast<C_Account*>(_raw));
}

std::string OlmAccount::pickle(const std::string& key) {
    auto* r = static_cast<C_Account*>(_raw);
    size_t len = olm_pickle_account_length(r);
    std::string pickled(len, '\0');
    size_t ret = olm_pickle_account(r,
        key.data(), key.size(), &pickled[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_pickle_account: ") +
                                 olm_account_last_error(r));
    }
    pickled.resize(ret);
    return pickled;
}

OlmAccount OlmAccount::unpickle(const std::string& key,
                                  const std::string& pickle) {
    OlmAccount account;
    auto* r = static_cast<C_Account*>(account._raw);
    size_t ret = olm_unpickle_account(r,
        key.data(), key.size(),
        const_cast<char*>(pickle.data()), pickle.size());
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_unpickle_account: ") +
                                 olm_account_last_error(r));
    }
    return account;
}

// ============================================================
// OlmSession
// ============================================================

OlmSession::OlmSession() {
    _buffer.resize(olm_session_size());
    _raw = olm_session(_buffer.data());
    if (!_raw)
        throw std::runtime_error("olm_session: failed to initialise");
}

OlmSession::~OlmSession() {
    if (_raw)
        olm_clear_session(static_cast<C_Session*>(_raw));
}

OlmSession::OlmSession(OlmSession&& other) noexcept
    : _buffer(std::move(other._buffer)), _raw(other._raw) {
    other._raw = nullptr;
}

OlmSession& OlmSession::operator=(OlmSession&& other) noexcept {
    if (this != &other) {
        if (_raw)
            olm_clear_session(static_cast<C_Session*>(_raw));
        _buffer = std::move(other._buffer);
        _raw = other._raw;
        other._raw = nullptr;
    }
    return *this;
}

void OlmSession::createOutbound(OlmAccount& account,
                                 const std::string& theirIdentityKey,
                                 const std::string& theirOneTimeKey) {
    auto ik = base64Decode(theirIdentityKey);
    auto otk = base64Decode(theirOneTimeKey);

    auto* s = static_cast<C_Session*>(_raw);
    size_t randLen = olm_create_outbound_session_random_length(s);
    auto random = randomBytes(randLen);

    size_t ret = olm_create_outbound_session(s,
        static_cast<C_Account*>(account.raw()),
        ik.data(), ik.size(), otk.data(), otk.size(),
        random.data(), randLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_create_outbound_session: ") +
                                 olm_session_last_error(s));
    }
}

void OlmSession::createInbound(OlmAccount& account,
                                const std::string& oneTimeKeyMessage) {
    auto msg = base64Decode(oneTimeKeyMessage);
    auto* s = static_cast<C_Session*>(_raw);
    size_t ret = olm_create_inbound_session(s,
        static_cast<C_Account*>(account.raw()),
        msg.data(), msg.size());
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_create_inbound_session: ") +
                                 olm_session_last_error(s));
    }
}

void OlmSession::createInboundFrom(OlmAccount& account,
                                    const std::string& theirIdentityKey,
                                    const std::string& oneTimeKeyMessage) {
    auto ik = base64Decode(theirIdentityKey);
    auto msg = base64Decode(oneTimeKeyMessage);
    auto* s = static_cast<C_Session*>(_raw);
    size_t ret = olm_create_inbound_session_from(s,
        static_cast<C_Account*>(account.raw()),
        ik.data(), ik.size(), msg.data(), msg.size());
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_create_inbound_session_from: ") +
                                 olm_session_last_error(s));
    }
}

bool OlmSession::matchesInbound(const std::string& oneTimeKeyMessage) {
    auto msg = base64Decode(oneTimeKeyMessage);
    auto* s = static_cast<C_Session*>(_raw);
    size_t ret = olm_matches_inbound_session(s, msg.data(), msg.size());
    if (ret == olm_error())
        return false;
    return ret == 1;
}

bool OlmSession::matchesInboundFrom(const std::string& theirIdentityKey,
                                     const std::string& oneTimeKeyMessage) {
    auto ik = base64Decode(theirIdentityKey);
    auto msg = base64Decode(oneTimeKeyMessage);
    auto* s = static_cast<C_Session*>(_raw);
    size_t ret = olm_matches_inbound_session_from(s,
        ik.data(), ik.size(), msg.data(), msg.size());
    if (ret == olm_error())
        return false;
    return ret == 1;
}

std::string OlmSession::sessionId() {
    auto* s = static_cast<C_Session*>(_raw);
    size_t len = olm_session_id_length(s);
    std::string id(len, '\0');
    size_t ret = olm_session_id(s, &id[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_session_id: ") +
                                 olm_session_last_error(s));
    }
    id.resize(ret);
    return id;
}

bool OlmSession::hasReceivedMessage() const {
    return olm_session_has_received_message(
        static_cast<const C_Session*>(_raw)) != 0;
}

size_t OlmSession::encryptMessageType() {
    auto* s = static_cast<C_Session*>(_raw);
    size_t msgType = olm_encrypt_message_type(s);
    if (msgType == olm_error()) {
        throw std::runtime_error(std::string("olm_encrypt_message_type: ") +
                                 olm_session_last_error(s));
    }
    return msgType;
}

std::string OlmSession::encrypt(const std::string& plaintext) {
    auto* s = static_cast<C_Session*>(_raw);
    size_t msgLen = olm_encrypt_message_length(s, plaintext.size());
    size_t randLen = olm_encrypt_random_length(s);
    auto random = randomBytes(randLen);

    std::vector<uint8_t> msgBuf(msgLen);
    size_t ret = olm_encrypt(s,
        reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size(),
        random.data(), randLen, msgBuf.data(), msgLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_encrypt: ") +
                                 olm_session_last_error(s));
    }
    msgBuf.resize(ret);
    return base64Encode(msgBuf.data(), msgBuf.size());
}

size_t OlmSession::decryptMaxPlaintextLength(size_t messageType,
                                              const std::string& ciphertext) {
    auto ct = base64Decode(ciphertext);
    auto* s = static_cast<C_Session*>(_raw);
    size_t maxLen = olm_decrypt_max_plaintext_length(
        s, messageType, ct.data(), ct.size());
    if (maxLen == olm_error()) {
        throw std::runtime_error(
            std::string("olm_decrypt_max_plaintext_length: ") +
            olm_session_last_error(s));
    }
    return maxLen;
}

std::string OlmSession::decrypt(size_t messageType,
                                 const std::string& ciphertext) {
    auto ct = base64Decode(ciphertext);
    auto* s = static_cast<C_Session*>(_raw);
    size_t maxLen = olm_decrypt_max_plaintext_length(
        s, messageType, ct.data(), ct.size());
    if (maxLen == olm_error()) {
        throw std::runtime_error(
            std::string("olm_decrypt_max_plaintext_length: ") +
            olm_session_last_error(s));
    }

    std::vector<uint8_t> plaintext(maxLen);
    size_t ret = olm_decrypt(s, messageType,
        ct.data(), ct.size(), plaintext.data(), maxLen);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_decrypt: ") +
                                 olm_session_last_error(s));
    }
    return std::string(plaintext.begin(), plaintext.begin() + ret);
}

void OlmSession::removeOneTimeKeys(OlmAccount& account) {
    auto* s = static_cast<C_Session*>(_raw);
    size_t ret = olm_remove_one_time_keys(
        static_cast<C_Account*>(account.raw()), s);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_remove_one_time_keys: ") +
                                 olm_session_last_error(s));
    }
}

std::string OlmSession::pickle(const std::string& key) {
    auto* s = static_cast<C_Session*>(_raw);
    size_t len = olm_pickle_session_length(s);
    std::string pickled(len, '\0');
    size_t ret = olm_pickle_session(s,
        key.data(), key.size(), &pickled[0], len);
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_pickle_session: ") +
                                 olm_session_last_error(s));
    }
    pickled.resize(ret);
    return pickled;
}

OlmSession OlmSession::unpickle(const std::string& key,
                                  const std::string& pickle) {
    OlmSession session;
    auto* s = static_cast<C_Session*>(session._raw);
    size_t ret = olm_unpickle_session(s,
        key.data(), key.size(),
        const_cast<char*>(pickle.data()), pickle.size());
    if (ret == olm_error()) {
        throw std::runtime_error(std::string("olm_unpickle_session: ") +
                                 olm_session_last_error(s));
    }
    return session;
}

}} // namespace matrixcli::e2ee

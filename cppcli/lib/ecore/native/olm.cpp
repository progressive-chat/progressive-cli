#include "progressive/olm.hpp"
#include <olm/olm.h>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <random>

namespace progressive {

// ==== Utility functions ====

std::string generateRandomBytes(int count) {
    std::string result(count, '\0');
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t total = 0;
        while (total < count) {
            ssize_t n = read(fd, &result[total], count - total);
            if (n <= 0) break;
            total += n;
        }
        close(fd);
        if ((size_t)total == (size_t)count) return result;
    }
    std::random_device rd;
    for (int i = 0; i < count; ++i) result[i] = static_cast<char>(rd());
    return result;
}

std::string olmErrorToString(OlmError error) {
    switch (error) {
        case OlmError::None: return "No error";
        case OlmError::NotEnoughRandom: return "Not enough random data";
        case OlmError::OutputBufferTooSmall: return "Output buffer too small";
        case OlmError::BadMessageVersion: return "Bad message version";
        case OlmError::BadMessageFormat: return "Bad message format";
        case OlmError::BadMessageMac: return "Bad message MAC";
        case OlmError::BadMessageKeyId: return "Bad message key ID";
        case OlmError::InvalidBase64: return "Invalid base64";
        case OlmError::BadAccountKey: return "Bad account key";
        case OlmError::UnknownPickleVersion: return "Unknown pickle version";
        case OlmError::Corruption: return "Data corruption";
        case OlmError::SessionNotFound: return "Session not found";
        default: return "Unknown error";
    }
}

std::string formatPickle(const std::string& type, const std::string& pickle) {
    std::ostringstream out;
    out << type << ":" << pickle;
    return out.str();
}

// ==== OlmAccount ====

OlmAccount::OlmAccount() {
    size_t sz = olm_account_size();
    account_ = new uint8_t[sz];
    memset(account_, 0, sz);
    olm_account(account_);
}

OlmAccount::~OlmAccount() {
    olm_clear_account(static_cast<::OlmAccount*>(account_));
    delete[] static_cast<uint8_t*>(account_);
}

OlmAccountResult OlmAccount::create() {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t randLen = olm_create_account_random_length(acc);
    auto random = generateRandomBytes(randLen);
    int rc = olm_create_account(acc, random.data(), random.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::NotEnoughRandom;
        return result;
    }
    result.success = true;
    return result;
}

OlmAccountResult OlmAccount::identityKeys() {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t len = olm_account_identity_keys_length(acc);
    std::string out(len, 0);
    size_t written = olm_account_identity_keys(acc, &out[0], len);
    if (written == static_cast<size_t>(-1)) {
        result.error = OlmError::OutputBufferTooSmall;
        return result;
    }
    out.resize(written);
    result.success = true;
    result.data = out;
    return result;
}

OlmAccountResult OlmAccount::generateOneTimeKeys(int count) {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t randLen = olm_account_generate_one_time_keys_random_length(acc, count);
    auto random = generateRandomBytes(randLen);
    int rc = olm_account_generate_one_time_keys(acc, count, random.data(), random.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::NotEnoughRandom;
        return result;
    }
    fprintf(stderr, "[e2ee] generateOneTimeKeys: generated %d keys, retrieving JSON...\n", (int)count);

    size_t kLen = olm_account_one_time_keys_length(acc);
    fprintf(stderr, "[e2ee] generateOneTimeKeys: JSON length=%zu\n", kLen);

    std::string keys(kLen, '\0');
    olm_account_one_time_keys(acc, &keys[0], kLen);

    fprintf(stderr, "[e2ee] generateOneTimeKeys: JSON first 100 chars=[%.100s]\n", keys.c_str());
    result.success = true;
    result.data = std::move(keys);
    return result;
}

OlmAccountResult OlmAccount::generateFallbackKey() {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t randLen = olm_account_generate_fallback_key_random_length(acc);
    auto random = generateRandomBytes(randLen);
    size_t rc = olm_account_generate_fallback_key(acc, random.data(), random.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::NotEnoughRandom;
        return result;
    }
    result.success = true;
    return result;
}

OlmAccountResult OlmAccount::unpublishedFallbackKey() {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t kLen = olm_account_unpublished_fallback_key_length(acc);
    if (kLen == 0) {
        result.success = true;
        return result;
    }
    std::string key(kLen, '\0');
    size_t rc = olm_account_unpublished_fallback_key(acc, &key[0], kLen);
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::UnknownError;
        return result;
    }
    result.success = true;
    // libolm always reports the buffer length of {"curve25519":{}} even when
    // no unpublished fallback key exists — detect the empty form by content.
    if (key.find(":{}") != std::string::npos) {
        return result;
    }
    result.data = std::move(key);
    return result;
}

OlmAccountResult OlmAccount::forgetOldFallbackKey() {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    ::olm_account_forget_old_fallback_key(acc);
    result.success = true;
    return result;
}

OlmAccountResult OlmAccount::markKeysAsPublished() {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t rc = ::olm_account_mark_keys_as_published(acc);
    result.success = (rc != ::olm_error());
    if (!result.success) {
        result.error = OlmError::UnknownError;
    }
    return result;
}

OlmAccountResult OlmAccount::sign(const std::string& message) {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t sigLen = olm_account_signature_length(acc);
    std::string sig(sigLen, 0);
    size_t written = olm_account_sign(acc, message.data(), message.size(), &sig[0], sigLen);
    if (written == static_cast<size_t>(-1)) {
        result.error = OlmError::OutputBufferTooSmall;
        return result;
    }
    sig.resize(written);
    result.success = true;
    result.data = sig;
    return result;
}

OlmAccountResult OlmAccount::pickle(const std::string& key) {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    size_t len = olm_pickle_account_length(acc);
    std::string out(len, 0);
    size_t written = olm_pickle_account(acc, key.data(), key.size(), &out[0], len);
    if (written == static_cast<size_t>(-1)) {
        result.error = OlmError::UnknownPickleVersion;
        return result;
    }
    out.resize(written);
    result.success = true;
    result.data = out;
    return result;
}

OlmAccountResult OlmAccount::unpickle(const std::string& key, const std::string& pickle) {
    OlmAccountResult result;
    auto* acc = static_cast<::OlmAccount*>(account_);
    int rc = olm_unpickle_account(acc, key.data(), key.size(),
        (void*)pickle.data(), pickle.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::BadAccountKey;
        return result;
    }
    result.success = true;
    return result;
}

OlmAccountResult OlmAccount::ed25519Key() {
    auto keys = identityKeys();
    if (!keys.success) return keys;
    auto pos = keys.data.find("\"ed25519\":\"");
    if (pos == std::string::npos) {
        keys.success = false;
        return keys;
    }
    pos += 11;
    auto end = keys.data.find('"', pos);
    keys.data = keys.data.substr(pos, end - pos);
    return keys;
}

OlmAccountResult OlmAccount::curve25519Key() {
    auto keys = identityKeys();
    if (!keys.success) return keys;
    auto pos = keys.data.find("\"curve25519\":\"");
    if (pos == std::string::npos) {
        keys.success = false;
        return keys;
    }
    pos += 14;
    auto end = keys.data.find('"', pos);
    keys.data = keys.data.substr(pos, end - pos);
    return keys;
}

int OlmAccount::maxOneTimeKeys() {
    auto* acc = static_cast<::OlmAccount*>(account_);
    return static_cast<int>(olm_account_max_number_of_one_time_keys(acc));
}

// ==== OlmSession ====

OlmSession::OlmSession() {
    size_t sz = olm_session_size();
    session_ = new uint8_t[sz];
    memset(session_, 0, sz);
    olm_session(session_);
}

OlmSession::~OlmSession() {
    olm_clear_session(static_cast<::OlmSession*>(session_));
    delete[] static_cast<uint8_t*>(session_);
}

OlmSessionResult OlmSession::createOutbound(OlmAccount& account,
    const std::string& theirIdentityKey, const std::string& theirOneTimeKey) {
    OlmSessionResult result;
    auto* sess = static_cast<::OlmSession*>(session_);
    auto* acc = static_cast<::OlmAccount*>(account.account_);
    size_t randLen = olm_create_outbound_session_random_length(sess);
    auto random = generateRandomBytes(randLen);
    size_t rc = olm_create_outbound_session(sess, acc,
        theirIdentityKey.data(), theirIdentityKey.size(),
        theirOneTimeKey.data(), theirOneTimeKey.size(),
        random.data(), random.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::BadMessageFormat;
        return result;
    }
    result.success = true;
    return result;
}

OlmSessionResult OlmSession::createInbound(OlmAccount& account, const std::string& preKeyMessage) {
    OlmSessionResult result;
    auto* sess = static_cast<::OlmSession*>(session_);
    auto* acc = static_cast<::OlmAccount*>(account.account_);
    // Real API: olm_create_inbound_session(session, account, msg, msg_len) — NO random bytes
    size_t rc = olm_create_inbound_session(sess, acc,
        (void*)preKeyMessage.data(), preKeyMessage.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::BadMessageFormat;
        return result;
    }
    result.success = true;
    return result;
}

OlmSessionResult OlmSession::createInboundFrom(OlmAccount& account,
    const std::string& theirIdentityKey, const std::string& encryptedMessage) {
    OlmSessionResult result;
    auto* sess = static_cast<::OlmSession*>(session_);
    auto* acc = static_cast<::OlmAccount*>(account.account_);
    size_t rc = olm_create_inbound_session_from(sess, acc,
        theirIdentityKey.data(), theirIdentityKey.size(),
        (void*)encryptedMessage.data(), encryptedMessage.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::BadMessageFormat;
        return result;
    }
    result.success = true;
    return result;
}

OlmSessionResult OlmSession::encrypt(const std::string& plaintext) {
    OlmSessionResult result;
    auto* sess = static_cast<::OlmSession*>(session_);

    // Real API: olm_encrypt(session, plaintext, pt_len, random, random_len, message, msg_len)
    size_t randLen = olm_encrypt_random_length(sess);
    auto random = generateRandomBytes(randLen);

    size_t msgLen = olm_encrypt_message_length(sess, plaintext.size());
    std::string msg(msgLen, 0);

    size_t written = olm_encrypt(sess, plaintext.data(), plaintext.size(),
        random.data(), random.size(), &msg[0], msgLen);
    if (written == static_cast<size_t>(-1)) {
        result.error = OlmError::OutputBufferTooSmall;
        return result;
    }
    msg.resize(written);
    result.success = true;
    result.data = msg;
    result.messageType = olm_encrypt_message_type(sess);
    return result;
}

OlmSessionResult OlmSession::decrypt(const std::string& encryptedMessage, int messageType) {
    OlmSessionResult result;
    auto* sess = static_cast<::OlmSession*>(session_);
    // libolm clobbers the message buffer in-place via b64_input — use mutable copy
    std::string msg(encryptedMessage);
    size_t ptLen = olm_decrypt_max_plaintext_length(sess, messageType,
        (void*)msg.data(), msg.size());
    if (ptLen == static_cast<size_t>(-1)) {
        std::fprintf(stderr, "[E2EE] OlmDecrypt: maxLen err=%s\n",
            ::olm_session_last_error(sess));
        result.error = OlmError::BadMessageFormat;
        return result;
    }
    std::string pt(ptLen, 0);
    msg = encryptedMessage;  // restore before second call
    size_t written = olm_decrypt(sess, messageType,
        (void*)msg.data(), msg.size(), &pt[0], ptLen);
    if (written == static_cast<size_t>(-1)) {
        std::fprintf(stderr, "[E2EE] OlmDecrypt: written err=%s\n",
            ::olm_session_last_error(sess));
        result.error = OlmError::BadMessageFormat;
        return result;
    }
    std::fprintf(stderr, "[E2EE] OlmDecrypt: written=%zu ok\n", written);
    pt.resize(written);
    result.success = true;
    result.data = pt;
    return result;
}

OlmSessionResult OlmSession::pickle(const std::string& key) {
    OlmSessionResult result;
    auto* sess = static_cast<::OlmSession*>(session_);
    size_t len = olm_pickle_session_length(sess);
    std::string out(len, 0);
    size_t written = olm_pickle_session(sess, key.data(), key.size(), &out[0], len);
    if (written == static_cast<size_t>(-1)) {
        result.error = OlmError::UnknownPickleVersion;
        return result;
    }
    out.resize(written);
    result.success = true;
    result.data = out;
    return result;
}

OlmSessionResult OlmSession::unpickle(const std::string& key, const std::string& pickle) {
    OlmSessionResult result;
    auto* sess = static_cast<::OlmSession*>(session_);
    int rc = olm_unpickle_session(sess, key.data(), key.size(),
        (void*)pickle.data(), pickle.size());
    if (rc == static_cast<size_t>(-1)) {
        result.error = OlmError::BadAccountKey;
        return result;
    }
    result.success = true;
    return result;
}

bool OlmSession::matchesInbound(const std::string& preKeyMessage) {
    auto* sess = static_cast<::OlmSession*>(session_);
    size_t rc = olm_matches_inbound_session(sess, (void*)preKeyMessage.data(), preKeyMessage.size());
    return rc == 1;
}

} // namespace progressive

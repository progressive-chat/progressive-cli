// src/core/crypto/backup_crypto.cpp
#include "backup_crypto.hpp"
#include "olm_account.hpp"

#include <sodium.h>
#include <vector>
#include <simdjson.h>

namespace progressive::desktop {

namespace {

// Byte-oriented helpers over the canonical string-based base64 (olm_account).
static std::vector<uint8_t> b64Decode(const std::string& in) {
    auto s = base64Decode(in);
    return std::vector<uint8_t>(s.begin(), s.end());
}
static std::string b64Encode(const uint8_t* d, size_t n) {
    return base64Encode(std::string(reinterpret_cast<const char*>(d), n));
}

bool sodiumReady() {
    static bool ok = []() { return sodium_init() >= 0; }();
    return ok;
}
} // namespace

std::string buildBackupVersionBody(const BackupVersionInfo& info) {
    return "{\"algorithm\":\"" + info.algorithm
        + "\",\"auth_data\":{\"public_key\":\"" + info.publicKey
        + "\",\"signatures\":{}}}";
}

std::string encryptBackupSessionData(const std::string& megolmExportBase64,
                                     const std::string& backupPublicKeyB64) {
    if (!sodiumReady()) return "";
    auto pk = b64Decode(backupPublicKeyB64);
    auto plaintext = b64Decode(megolmExportBase64);
    if (pk.size() != crypto_box_PUBLICKEYBYTES || plaintext.empty()) return "";

    std::vector<uint8_t> ciphertext(plaintext.size() + crypto_box_SEALBYTES);
    if (crypto_box_seal(ciphertext.data(), plaintext.data(), plaintext.size(),
                        pk.data()) != 0) return "";

    // session_data: {"ephemeral":<b64>, "ciphertext":<b64>, "mac":<b64>}
    // crypto_box_seal embeds the ephemeral pubkey in the first 32 bytes.
    std::string ephemeral = b64Encode(ciphertext.data(), crypto_box_PUBLICKEYBYTES);
    std::string body = b64Encode(ciphertext.data() + crypto_box_PUBLICKEYBYTES,
                                 ciphertext.size() - crypto_box_PUBLICKEYBYTES);
    return "{\"ephemeral\":\"" + ephemeral + "\",\"ciphertext\":\"" + body
        + "\",\"mac\":\"\"}";
}

std::string decryptBackupSessionData(const std::string& sessionDataJson,
                                     const std::string& backupPrivateKeyB64) {
    if (!sodiumReady()) return "";
    auto sk = b64Decode(backupPrivateKeyB64);
    if (sk.size() != crypto_box_SECRETKEYBYTES) return "";

    simdjson::dom::parser p;
    auto doc = p.parse(sessionDataJson);
    if (doc.error() != simdjson::SUCCESS) return "";

    auto ep = doc.value()["ephemeral"].get_string();
    auto ct = doc.value()["ciphertext"].get_string();
    if (ep.error() != simdjson::SUCCESS || ct.error() != simdjson::SUCCESS) return "";

    std::vector<uint8_t> combined;
    {
        auto e = b64Decode(std::string(ep.value()));
        auto c = b64Decode(std::string(ct.value()));
        if (e.size() != crypto_box_PUBLICKEYBYTES) return "";
        combined = e;
        combined.insert(combined.end(), c.begin(), c.end());
    }

    std::vector<uint8_t> plaintext(combined.size() - crypto_box_SEALBYTES);
    // NOTE: some AArch64 libsodium builds segfault on seal_open with a NULL
    // pk AND use the pk's VALUE — derive the real recipient public key from
    // the secret and pass it.
    unsigned char recipientPk[crypto_box_PUBLICKEYBYTES];
    if (crypto_scalarmult_curve25519_base(recipientPk, sk.data()) != 0) return "";
    if (crypto_box_seal_open(plaintext.data(), combined.data(), combined.size(),
                             recipientPk, sk.data()) != 0) return "";
    return b64Encode(plaintext.data(), plaintext.size());
}

std::string buildBackupSessionEntry(const std::string& sessionDataJson,
                                    int firstMessageIndex) {
    return "{\"first_message_index\":" + std::to_string(firstMessageIndex)
        + ",\"forwarded_count\":0,\"is_verified\":false"
        + ",\"session_data\":" + sessionDataJson + "}";
}

} // namespace progressive::desktop

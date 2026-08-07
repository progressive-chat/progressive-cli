// src/core/crypto/recovery_key.cpp
#include "recovery_key.hpp"

#include <sodium.h>
#include <cstring>
#include <cstdlib>

namespace progressive::desktop {

namespace {
static const char kB58[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool sodiumInitialized() {
    static bool ok = []() { return sodium_init() >= 0; }();
    return ok;
}
} // namespace

std::string base58Encode(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> digits;
    for (uint8_t b : data) {
        int carry = b;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            carry += (*it) << 8;
            *it = carry % 58;
            carry /= 58;
        }
        while (carry > 0) {
            digits.insert(digits.begin(), static_cast<uint8_t>(carry % 58));
            carry /= 58;
        }
    }
    std::string out;
    for (uint8_t b : data) {
        if (b == 0) out += '1';
        else break;
    }
    for (uint8_t d : digits) out += kB58[d];
    return out;
}

std::vector<uint8_t> base58Decode(const std::string& input) {
    std::vector<uint8_t> bytes;
    for (char c : input) {
        const char* p = std::strchr(kB58, c);
        if (!p) return {};
        int carry = static_cast<int>(p - kB58);
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
            carry += (*it) * 58;
            *it = static_cast<uint8_t>(carry & 0xFF);
            carry >>= 8;
        }
        while (carry > 0) {
            bytes.insert(bytes.begin(), static_cast<uint8_t>(carry & 0xFF));
            carry >>= 8;
        }
    }
    for (char c : input) {
        if (c == '1') bytes.insert(bytes.begin(), 0);
        else break;
    }
    return bytes;
}

std::string generateRecoveryKey() {
    if (!sodiumInitialized()) return "";
    std::vector<uint8_t> seed(32);
    randombytes_buf(seed.data(), seed.size());
    uint8_t parity = 0;
    for (uint8_t b : seed) parity = static_cast<uint8_t>(parity + b);
    std::vector<uint8_t> full(seed);
    full.push_back(parity);
    full.push_back(parity);
    return base58Encode(full);
}

bool isValidRecoveryKey(const std::string& key) {
    auto decoded = base58Decode(key);
    if (decoded.size() != 34) return false;
    uint8_t parity = 0;
    for (size_t i = 0; i < 32; ++i) parity = static_cast<uint8_t>(parity + decoded[i]);
    return decoded[32] == parity && decoded[33] == parity;
}

std::vector<uint8_t> recoveryKeySeed(const std::string& key) {
    auto decoded = base58Decode(key);
    if (decoded.size() < 32) return {};
    return std::vector<uint8_t>(decoded.begin(), decoded.begin() + 32);
}

BackupKeyPair deriveBackupKey(const std::vector<uint8_t>& seed) {
    BackupKeyPair pair;
    if (!sodiumInitialized() || seed.size() != 32) return pair;
    // Per m.megolm_backup.v1 the recovery seed IS the curve25519 backup
    // private key (no ed25519 conversion — crypto_sign_ed25519_seed_keypair
    // also crashes on some AArch64 libsodium builds).
    unsigned char curveSk[crypto_scalarmult_curve25519_BYTES];
    std::memcpy(curveSk, seed.data(), 32);
    unsigned char curvePk[crypto_scalarmult_curve25519_BYTES];
    if (crypto_scalarmult_curve25519_base(curvePk, curveSk) != 0) return pair;
    auto enc = [](const unsigned char* d, size_t n) {
        char* out = static_cast<char*>(malloc(sodium_base64_encoded_len(
            n, sodium_base64_VARIANT_ORIGINAL)));
        if (!out) return std::string();
        sodium_bin2base64(out, sodium_base64_encoded_len(n,
            sodium_base64_VARIANT_ORIGINAL), d, n, sodium_base64_VARIANT_ORIGINAL);
        std::string s(out);
        free(out);
        return s;
    };
    pair.privateKeyB64 = enc(curveSk, sizeof(curveSk));
    pair.publicKeyB64 = enc(curvePk, sizeof(curvePk));
    return pair;
}

} // namespace progressive::desktop

// src/core/crypto/ssss.cpp
#include "ssss.hpp"

#include "olm_account.hpp"
#include "recovery_key.hpp"

#include "progressive/crypto_algorithms.hpp"

#include <openssl/evp.h>
#include <sodium.h>
#include <simdjson.h>
#include <cstring>
#include <vector>

namespace progressive::desktop {

namespace {

// libolm-backed SHA/HMAC/HKDF (progressive_native, namespace progressive).
std::vector<uint8_t> hmacSha256(const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& data) {
    return progressive::hmacSha256(key.data(), key.size(), data.data(), data.size());
}

std::vector<uint8_t> hkdfSha256(const std::vector<uint8_t>& ikm,
                                const std::vector<uint8_t>& salt,
                                const std::vector<uint8_t>& info, int len) {
    return progressive::hkdfDeriveSecret(ikm, salt, info, len);
}

std::vector<uint8_t> aesCbcEncrypt(const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& iv,
                                   const std::vector<uint8_t>& plaintext) {
    std::vector<uint8_t> out(plaintext.size() + EVP_CIPHER_block_size(EVP_aes_256_cbc()));
    int outLen = 0, finalLen = 0;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    if (EVP_EncryptUpdate(ctx, out.data(), &outLen, plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    if (EVP_EncryptFinal_ex(ctx, out.data() + outLen, &finalLen) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    EVP_CIPHER_CTX_free(ctx);
    out.resize(outLen + finalLen);
    return out;
}

std::vector<uint8_t> aesCbcDecrypt(const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& iv,
                                   const std::vector<uint8_t>& ciphertext) {
    std::vector<uint8_t> out(ciphertext.size() + EVP_CIPHER_block_size(EVP_aes_256_cbc()));
    int outLen = 0, finalLen = 0;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    if (EVP_DecryptUpdate(ctx, out.data(), &outLen, ciphertext.data(),
                          static_cast<int>(ciphertext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    if (EVP_DecryptFinal_ex(ctx, out.data() + outLen, &finalLen) != 1) {
        EVP_CIPHER_CTX_free(ctx); return {};
    }
    EVP_CIPHER_CTX_free(ctx);
    out.resize(outLen + finalLen);
    return out;
}

const char* kInfo = "m.secret_storage.v1.aes-hmac-sha2";

// olm_account's base64 is string-based — byte helpers for the vectors here.
std::vector<uint8_t> b64Bytes(const std::string& in) {
    auto s = base64Decode(in);
    return std::vector<uint8_t>(s.begin(), s.end());
}
std::string b64Str(const std::vector<uint8_t>& v) {
    return base64Encode(std::string(v.begin(), v.end()));
}

} // namespace

bool deriveSsssKeys(const std::vector<uint8_t>& seed, const std::string& keyId,
                    std::vector<uint8_t>& aesKey, std::vector<uint8_t>& hmacKey) {
    if (seed.size() != 32) return false;
    std::vector<uint8_t> salt(keyId.begin(), keyId.end());
    std::vector<uint8_t> info(kInfo, kInfo + std::strlen(kInfo));
    auto derived = hkdfSha256(seed, salt, info, 64);
    if (derived.size() != 64) return false;
    aesKey.assign(derived.begin(), derived.begin() + 32);
    hmacKey.assign(derived.begin() + 32, derived.end());
    return true;
}

std::string encryptSsssSecret(const std::string& plaintext,
                              const std::vector<uint8_t>& aesKey,
                              const std::vector<uint8_t>& hmacKey) {
    if (aesKey.size() != 32 || hmacKey.size() != 32) return "";
    std::vector<uint8_t> iv(16);
    randombytes_buf(iv.data(), iv.size());
    std::vector<uint8_t> pt(plaintext.begin(), plaintext.end());
    auto ct = aesCbcEncrypt(aesKey, iv, pt);
    if (ct.empty()) return "";

    std::vector<uint8_t> macInput = iv;
    macInput.insert(macInput.end(), ct.begin(), ct.end());
    auto mac = hmacSha256(hmacKey, macInput);
    if (mac.size() < 8) return "";
    mac.resize(8);  // spec: the MAC is truncated to 8 bytes

    return "{\"iv\":\"" + b64Str(iv)
        + "\",\"ciphertext\":\"" + b64Str(ct)
        + "\",\"mac\":\"" + b64Str(mac) + "\"}";
}

std::string decryptSsssSecret(const std::string& secretJson,
                              const std::vector<uint8_t>& aesKey,
                              const std::vector<uint8_t>& hmacKey) {
    if (aesKey.size() != 32 || hmacKey.size() != 32) return "";
    simdjson::dom::parser p;
    auto doc = p.parse(secretJson);
    if (doc.error() != simdjson::SUCCESS) return "";

    auto ivS = doc.value()["iv"].get_string();
    auto ctS = doc.value()["ciphertext"].get_string();
    auto macS = doc.value()["mac"].get_string();
    if (ivS.error() != simdjson::SUCCESS || ctS.error() != simdjson::SUCCESS ||
        macS.error() != simdjson::SUCCESS) return "";

    auto iv = b64Bytes(std::string(ivS.value()));
    auto ct = b64Bytes(std::string(ctS.value()));
    auto mac = b64Bytes(std::string(macS.value()));
    if (iv.size() != 16 || ct.empty() || mac.size() != 8) return "";

    // Verify the MAC first.
    std::vector<uint8_t> macInput = iv;
    macInput.insert(macInput.end(), ct.begin(), ct.end());
    auto calc = hmacSha256(hmacKey, macInput);
    if (calc.size() < 8) return "";
    calc.resize(8);
    if (calc != mac) return "";

    auto pt = aesCbcDecrypt(aesKey, iv, ct);
    if (pt.empty()) return "";
    return std::string(pt.begin(), pt.end());
}

std::string buildSsssKeyMetadata(const std::vector<uint8_t>& aesKey,
                                 const std::vector<uint8_t>& hmacKey) {
    // Self-encryption: the derived key encrypted with itself verifies the
    // derivation on another device.
    std::string self = encryptSsssSecret(
        b64Str(aesKey), aesKey, hmacKey);
    if (self.empty()) return "";
    // The metadata carries the iv + mac (the ciphertext of the self-encryption
    // is not stored — the mac over iv+ct verifies the key).
    simdjson::dom::parser p;
    auto doc = p.parse(self);
    if (doc.error() != simdjson::SUCCESS) return "";
    auto ivS = doc.value()["iv"].get_string();
    auto macS = doc.value()["mac"].get_string();
    if (ivS.error() != simdjson::SUCCESS || macS.error() != simdjson::SUCCESS) return "";
    return "{\"algorithm\":\"m.secret_storage.v1.aes-hmac-sha2\","
        "\"iv\":\"" + std::string(ivS.value()) + "\","
        "\"mac\":\"" + std::string(macS.value()) + "\"}";
}

bool verifySsssRecoveryKey(const std::string& metadataJson,
                           const std::vector<uint8_t>& aesKey,
                           const std::vector<uint8_t>& hmacKey) {
    simdjson::dom::parser p;
    auto doc = p.parse(metadataJson);
    if (doc.error() != simdjson::SUCCESS) return false;
    auto ivS = doc.value()["iv"].get_string();
    auto macS = doc.value()["mac"].get_string();
    if (ivS.error() != simdjson::SUCCESS || macS.error() != simdjson::SUCCESS) return false;

    // Rebuild the self-encryption with a KNOWN ciphertext: the metadata only
    // has iv+mac, so re-encrypt the key and compare the mac over [iv][ct].
    auto iv = b64Bytes(std::string(ivS.value()));
    auto mac = b64Bytes(std::string(macS.value()));
    if (iv.size() != 16 || mac.size() != 8) return false;
    // The metadata self-encryption used the key's BASE64 string as the
    // plaintext — re-encrypt the SAME plaintext for the mac comparison.
    std::string keyB64 = b64Str(aesKey);
    std::vector<uint8_t> pt(keyB64.begin(), keyB64.end());
    auto ct = aesCbcEncrypt(aesKey, iv, pt);
    if (ct.empty()) return false;
    std::vector<uint8_t> macInput = iv;
    macInput.insert(macInput.end(), ct.begin(), ct.end());
    auto calc = hmacSha256(hmacKey, macInput);
    if (calc.size() < 8) return false;
    calc.resize(8);
    return calc == mac;
}

std::string generateSsssKeyId() {
    std::vector<uint8_t> rnd(24);
    randombytes_buf(rnd.data(), rnd.size());
    return base64Encode(std::string(rnd.begin(), rnd.end()));
}

} // namespace progressive::desktop

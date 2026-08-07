// src/core/crypto/media_crypto.cpp — encrypted-media crypto (m.encrypted v2).
#include "media_crypto.hpp"

#include "core/crypto/olm_account.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace progressive::desktop {

bool generateMediaKeyIv(std::string& keyB64, std::string& ivB64) {
    std::vector<uint8_t> key(32), iv(16);
    if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) return false;
    if (RAND_bytes(iv.data(), static_cast<int>(iv.size())) != 1) return false;
    keyB64 = base64Encode(std::string(key.begin(), key.end()));
    ivB64 = base64Encode(std::string(iv.begin(), iv.end()));
    return !keyB64.empty() && !ivB64.empty();
}

std::vector<uint8_t> aesCtrCrypt(const std::vector<uint8_t>& data,
                                 const std::string& keyB64,
                                 const std::string& ivB64) {
    std::string key = base64Decode(keyB64);
    std::string iv = base64Decode(ivB64);
    if (key.size() != 32 || iv.size() != 16) return {};
    std::vector<uint8_t> out(data.size());
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    if (EVP_CipherInit_ex(ctx, EVP_aes_256_ctr(), nullptr,
                          reinterpret_cast<const unsigned char*>(key.data()),
                          reinterpret_cast<const unsigned char*>(iv.data()),
                          1) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    int outLen = 0, finalLen = 0;
    if (EVP_CipherUpdate(ctx, out.data(), &outLen, data.data(),
                         static_cast<int>(data.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    if (EVP_CipherFinal_ex(ctx, out.data() + outLen, &finalLen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }
    EVP_CIPHER_CTX_free(ctx);
    out.resize(static_cast<size_t>(outLen + finalLen));
    return out;
}

std::string sha256Base64(const std::vector<uint8_t>& data) {
    unsigned char digest[32];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);
    return base64Encode(std::string(reinterpret_cast<char*>(digest), len));
}

std::vector<uint8_t> decryptMedia(const std::vector<uint8_t>& ciphertext,
                                  const std::string& keyB64,
                                  const std::string& ivB64,
                                  const std::string& shaB64) {
    if (keyB64.empty() || ivB64.empty() || ciphertext.empty()) return {};
    auto plain = aesCtrCrypt(ciphertext, keyB64, ivB64);
    if (plain.empty()) return {};
    if (!shaB64.empty() && sha256Base64(plain) != shaB64) return {};
    return plain;
}

} // namespace progressive::desktop

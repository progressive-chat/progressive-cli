// src/core/crypto/ssss.hpp — m.secret_storage.v1.aes-hmac-sha2 (Phase 7).
// Cross-device secret sharing: the cross-signing private keys encrypted to
// account-data, unlockable with the recovery key.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace progressive::desktop {

// Derive the AES-256 + HMAC-SHA256 keys from the recovery seed + key id:
// HKDF-SHA256(ikm = seed, salt = keyId, info = "m.secret_storage.v1.aes-hmac-sha2")
// -> 64 bytes: first 32 = AES key, last 32 = HMAC key.
bool deriveSsssKeys(const std::vector<uint8_t>& seed, const std::string& keyId,
                    std::vector<uint8_t>& aesKey, std::vector<uint8_t>& hmacKey);

// Encrypt a secret (AES-256-CBC with a random 16-byte IV + HMAC-SHA256 over
// [iv][ciphertext] truncated to 8 bytes) -> {"iv":..,"ciphertext":..,"mac":..}.
std::string encryptSsssSecret(const std::string& plaintext,
                              const std::vector<uint8_t>& aesKey,
                              const std::vector<uint8_t>& hmacKey);

// Decrypt an m.secret_storage secret JSON. Returns "" on any failure.
std::string decryptSsssSecret(const std::string& secretJson,
                              const std::vector<uint8_t>& aesKey,
                              const std::vector<uint8_t>& hmacKey);

// The key-metadata JSON (m.secret_storage.key.<keyId>):
// {"algorithm":"m.secret_storage.v1.aes-hmac-sha2","iv":..,"mac":..} — the
// iv/mac verify the derivation by encrypting the key with itself.
std::string buildSsssKeyMetadata(const std::vector<uint8_t>& aesKey,
                                 const std::vector<uint8_t>& hmacKey);

// Verify a recovery key against the stored metadata (re-derive + compare mac).
bool verifySsssRecoveryKey(const std::string& metadataJson,
                           const std::vector<uint8_t>& aesKey,
                           const std::vector<uint8_t>& hmacKey);

// A random key id (unpadded base64 of 24 random bytes, 32 chars).
std::string generateSsssKeyId();

} // namespace progressive::desktop

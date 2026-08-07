// src/core/crypto/media_crypto.hpp — encrypted-media helpers (m.encrypted v2).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace progressive::desktop {

// m.encrypted v2 media: AES-256-CTR with a random 32-byte key + 16-byte IV
// (both base64). hashes.sha256 covers the PLAINTEXT (matrix-js-sdk
// semantics) — verify AFTER decryption.
bool generateMediaKeyIv(std::string& keyB64, std::string& ivB64);

// CTR is symmetric: encrypt == decrypt with the same key/iv.
std::vector<uint8_t> aesCtrCrypt(const std::vector<uint8_t>& data,
                                 const std::string& keyB64,
                                 const std::string& ivB64);

// Base64 SHA-256 of the data.
std::string sha256Base64(const std::vector<uint8_t>& data);

// Decrypt + verify the sha256 (base64). Empty on failure (bad key / tampered).
std::vector<uint8_t> decryptMedia(const std::vector<uint8_t>& ciphertext,
                                  const std::string& keyB64,
                                  const std::string& ivB64,
                                  const std::string& shaB64);

} // namespace progressive::desktop

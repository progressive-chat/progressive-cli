#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace progressive {

// ==== SHA-256 primitives (thin wrapper over libolm's sha256) ====
// These are declared in olm/crypto.hh but exposed here for module use.

// Compute SHA-256 of data, return 32-byte digest.
std::vector<uint8_t> sha256(const uint8_t* data, size_t len);

// Compute SHA-256 of string data.
inline std::vector<uint8_t> sha256(const std::string& data) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// ==== HMAC-SHA256 ====
//
// RFC 2104: HMAC(K, m) = H((K' ⊕ opad) || H((K' ⊕ ipad) || m))
// where K' = key padded/truncated to 64-byte block size.
//
// Original Kotlin (HkdfSha256.kt:96-100):
//   private fun initMac(secret: ByteArray): Mac {
//       val mac = Mac.getInstance("HmacSHA256")
//       mac.init(SecretKeySpec(secret, "HmacSHA256"))
//       return mac
//   }

std::vector<uint8_t> hmacSha256(const uint8_t* key, size_t keyLen,
                                 const uint8_t* data, size_t dataLen);

inline std::vector<uint8_t> hmacSha256(const std::vector<uint8_t>& key,
                                        const std::vector<uint8_t>& data) {
    return hmacSha256(key.data(), key.size(), data.data(), data.size());
}

// ==== HKDF-SHA256 (RFC 5869) ====
//
// HMAC-based Extract-and-Expand Key Derivation Function.
//
// Original Kotlin (HkdfSha256.kt:27-91):
//   fun deriveSecret(IKM, salt, info, outputLength): ByteArray
//   private fun extract(salt, IKM): ByteArray       // HKDF-Extract: PRK = HMAC-Hash(salt, IKM)
//   private fun expand(PRK, info, outputLength)     // HKDF-Expand: OKM = T(1)|T(2)|...
//
// RFC 5869 specification:
//   HKDF-Extract(salt, IKM) → PRK = HMAC-Hash(salt, IKM)
//   HKDF-Expand(PRK, info, L) → OKM:
//     N = ceil(L / HashLen)
//     T(0) = "" (empty)
//     T(i) = HMAC-Hash(PRK, T(i-1) || info || i)  for i = 1..N
//     OKM = first L bytes of T(1) || T(2) || ... || T(N)
//
// HashLen = 32 (SHA-256 output length)

constexpr int HKDF_HASH_LEN = 32; // SHA-256 output = 32 bytes

// Full HKDF: extract + expand in one call.
// salt: optional (pass empty vector for zeros of HashLen)
// info: optional context string
// outputLength: desired key length in bytes (≤ 255 * HashLen = 8160)
std::vector<uint8_t> hkdfDeriveSecret(
    const std::vector<uint8_t>& inputKeyMaterial,
    const std::vector<uint8_t>& salt,
    const std::vector<uint8_t>& info,
    int outputLength);

// HKDF-Extract: PRK = HMAC-SHA256(salt || zeros(32), IKM)
std::vector<uint8_t> hkdfExtract(
    const std::vector<uint8_t>& salt,
    const std::vector<uint8_t>& ikm);

// HKDF-Expand: OKM = T(1) || T(2) || ... truncated to outputLength
std::vector<uint8_t> hkdfExpand(
    const std::vector<uint8_t>& prk,
    const std::vector<uint8_t>& info,
    int outputLength);

// ==== Base58 Encode/Decode (Bitcoin-style) ====
//
// Alphabet: 123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz
// (excludes 0, O, I, l for readability)
//
// Original Kotlin (Base58.kt:22-86):
//   fun base58encode(input: ByteArray): String
//   fun base58decode(input: String): ByteArray
//
// Uses BigInt arithmetic (mod 58, div 58).

// Encode bytes to Base58 string.
// Leading zero bytes → leading '1' characters.
std::string base58Encode(const std::vector<uint8_t>& input);

// Decode Base58 string to bytes.
// Strips leading '1' padding.
std::vector<uint8_t> base58Decode(const std::string& input);

// ==== BigInt helpers for Base58 ====
// Simple decimal-string BigInt for mod/div by 58.
// Uses string-based arithmetic (no external deps), similar to string_order.hpp.

std::string bigIntMod58(const std::string& num, int* remainder);
std::string bigIntDiv58(const std::string& num, int* remainder);
int bigIntToInt(const std::string& num);

// Convert byte array to BigInteger string (positive, big-endian).
std::string bytesToBigInt(const std::vector<uint8_t>& bytes);

// Convert BigInteger string to byte array.
std::vector<uint8_t> bigIntToBytes(const std::string& bi);

// ==== MD5 Hash ====
//
// Original Kotlin (Hash.kt:21-34): String.md5() using MessageDigest
// Falls back to SHA-256 hex since NDK 21 libolm doesn't provide MD5.
// For real MD5, link against OpenSSL.

inline std::string md5Hash(const std::string& input) {
    auto hash = sha256(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    std::string hex;
    for (uint8_t b : hash) { hex += "0123456789abcdef"[b>>4]; hex += "0123456789abcdef"[b&0xf]; }
    return hex;
}

} // namespace progressive

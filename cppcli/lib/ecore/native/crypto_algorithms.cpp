#include "progressive/crypto_algorithms.hpp"
#include <olm/crypto.h>
#include <cstring>
#include <algorithm>

namespace progressive {

// ==== SHA-256 (via libolm) ====

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
    std::vector<uint8_t> result(32);
    _olm_crypto_sha256(data, len, result.data());
    return result;
}

// ==== HMAC-SHA256 (via libolm) ====

std::vector<uint8_t> hmacSha256(const uint8_t* key, size_t keyLen,
                                 const uint8_t* data, size_t dataLen) {
    std::vector<uint8_t> result(32);
    _olm_crypto_hmac_sha256(key, keyLen, data, dataLen, result.data());
    return result;
}

// ==== HKDF-SHA256 ====

std::vector<uint8_t> hkdfDeriveSecret(
    const std::vector<uint8_t>& ikm,
    const std::vector<uint8_t>& salt,
    const std::vector<uint8_t>& info,
    int outputLength)
{
    std::vector<uint8_t> result(outputLength);
    const uint8_t* saltPtr = salt.empty() ? nullptr : salt.data();
    _olm_crypto_hkdf_sha256(
        ikm.data(), ikm.size(),
        saltPtr, salt.size(),
        info.data(), info.size(),
        result.data(), outputLength);
    return result;
}

std::vector<uint8_t> hkdfExtract(const std::vector<uint8_t>& salt, const std::vector<uint8_t>& ikm) {
    const uint8_t* saltPtr = salt.empty() ? nullptr : salt.data();
    return hmacSha256(saltPtr, salt.size(), ikm.data(), ikm.size());
}

std::vector<uint8_t> hkdfExpand(
    const std::vector<uint8_t>& prk,
    const std::vector<uint8_t>& info,
    int outputLength)
{
    std::vector<uint8_t> result;
    result.reserve(outputLength);
    int n = (outputLength + HKDF_HASH_LEN - 1) / HKDF_HASH_LEN;
    std::vector<uint8_t> stepHash;
    for (int round = 1; round <= n && (int)result.size() < outputLength; round++) {
        std::vector<uint8_t> hmacInput;
        hmacInput.insert(hmacInput.end(), stepHash.begin(), stepHash.end());
        hmacInput.insert(hmacInput.end(), info.begin(), info.end());
        hmacInput.push_back(static_cast<uint8_t>(round));
        stepHash = hmacSha256(prk.data(), prk.size(), hmacInput.data(), hmacInput.size());
        int toCopy = std::min((int)stepHash.size(), outputLength - (int)result.size());
        result.insert(result.end(), stepHash.begin(), stepHash.begin() + toCopy);
    }
    return result;
}

// ==== Base58 ====
// Manual decimal-string BigInt implementation — no external deps.
// Original Kotlin (Base58.kt:22-86) uses java.math.BigInteger.

static const char* B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static void bigIntDivMod58(const std::string& num, std::string& quotient, int& remainder) {
    remainder = 0;
    quotient.clear();
    bool started = false;
    for (char c : num) {
        int value = remainder * 10 + (c - '0');
        int q = value / 58;
        int r = value % 58;
        if (q > 0 || started) { quotient += (char)('0' + q); started = true; }
        remainder = r;
    }
    if (quotient.empty()) quotient = "0";
}

static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::string hex;
    hex.reserve(bytes.size() * 2);
    const char* hexChars = "0123456789abcdef";
    for (uint8_t b : bytes) { hex += hexChars[b >> 4]; hex += hexChars[b & 0xf]; }
    return hex;
}

static std::string hexToDecimal(const std::string& hex) {
    std::string result = "0";
    for (char c : hex) {
        int digit = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c - 'A' + 10);
        int carry = digit;
        for (int i = (int)result.size() - 1; i >= 0; i--) {
            int val = (result[i] - '0') * 16 + carry;
            result[i] = '0' + (val % 10);
            carry = val / 10;
        }
        while (carry > 0) { result.insert(result.begin(), '0' + (carry % 10)); carry /= 10; }
    }
    return result;
}

std::string base58Encode(const std::vector<uint8_t>& input) {
    if (input.empty()) return "";
    std::string decimal = hexToDecimal(bytesToHex(input));
    std::string result;
    std::string num = decimal;
    while (num != "0") {
        int rem; std::string quot;
        bigIntDivMod58(num, quot, rem);
        result.insert(result.begin(), B58_ALPHABET[rem]);
        num = quot;
    }
    for (uint8_t b : input) {
        if (b == 0) result.insert(result.begin(), '1');
        else break;
    }
    return result;
}

std::vector<uint8_t> base58Decode(const std::string& input) {
    if (input.empty()) return {};
    std::string decimal = "0";
    for (char c : input) {
        const char* pos = strchr(B58_ALPHABET, c);
        if (!pos) continue;
        int digit = (int)(pos - B58_ALPHABET);
        int carry = digit;
        for (int i = (int)decimal.size() - 1; i >= 0; i--) {
            int val = (decimal[i] - '0') * 58 + carry;
            decimal[i] = '0' + (val % 10);
            carry = val / 10;
        }
        while (carry > 0) { decimal.insert(decimal.begin(), '0' + (carry % 10)); carry /= 10; }
    }
    std::vector<uint8_t> result;
    std::string num = decimal;
    while (num != "0" && !num.empty()) {
        int rem = 0; std::string quot; bool started = false;
        for (char c : num) {
            int val = rem * 10 + (c - '0');
            int q = val / 256; rem = val % 256;
            if (q > 0 || started) { quot += (char)('0' + q); started = true; }
        }
        if (quot.empty()) quot = "0";
        result.insert(result.begin(), (uint8_t)rem);
        num = quot;
    }
    int leadingZeros = 0;
    for (char c : input) { if (c == '1') leadingZeros++; else break; }
    result.insert(result.begin(), leadingZeros, 0);
    return result;
}

} // namespace progressive

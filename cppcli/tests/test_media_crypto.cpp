// tests/test_media_crypto.cpp — Qt-free encrypted-media crypto tests
// (m.encrypted v2: AES-256-CTR key/iv + sha256-of-plaintext verification).
#include "core/crypto/media_crypto.hpp"
#include "core/crypto/olm_account.hpp"

#include <iostream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

using namespace progressive::desktop;

int main() {
    std::string k1, iv1, k2, iv2;
    CHECK(generateMediaKeyIv(k1, iv1), "gen: first key+iv produced");
    CHECK(generateMediaKeyIv(k2, iv2), "gen: second key+iv produced");
    CHECK(k1 != k2 && iv1 != iv2, "gen: pairs are unique");
    CHECK(base64Decode(k1).size() == 32 && base64Decode(iv1).size() == 16,
          "gen: 32-byte key / 16-byte iv");

    std::vector<uint8_t> plain;
    for (int i = 0; i < 1000; ++i) plain.push_back(static_cast<uint8_t>((i * 31) & 0xff));
    auto cipher = aesCtrCrypt(plain, k1, iv1);
    CHECK(!cipher.empty() && cipher != plain, "ctr: ciphertext differs from plaintext");
    auto back = aesCtrCrypt(cipher, k1, iv1);
    CHECK(back == plain, "ctr: round trip restores the plaintext");

    std::string sha = sha256Base64(plain);
    CHECK(sha256Base64(plain) == sha, "sha: deterministic");
    CHECK(sha256Base64(cipher) != sha, "sha: hashes plaintext, not ciphertext");

    CHECK(decryptMedia(cipher, k2, iv1, sha).empty(), "dec: wrong key rejected");
    CHECK(decryptMedia(cipher, k1, iv1, sha256Base64(std::vector<uint8_t>{1, 2, 3})).empty(),
          "dec: wrong sha rejected");
    CHECK(decryptMedia(cipher, k1, iv1, sha) == plain, "dec: right key + sha -> plaintext");
    CHECK(decryptMedia(cipher, k1, iv1, "") == plain, "dec: empty sha skips verification");
    CHECK(decryptMedia(cipher, "", iv1, sha).empty(), "dec: empty key rejected");

    std::cout << (failures == 0 ? "ALL MEDIA CRYPTO TESTS PASSED\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}

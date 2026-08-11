// tests/test_e2ee_sas.cpp — SAS crypto roundtrip (Bug 1 + Bug 2 regression guard).
#include "core/crypto/sas.hpp"
#include "core/crypto/sas_emojis.hpp"
#include <iostream>
#include <string>
#include <cstring>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)
#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (expected " << (b) << " got " << (a) << ") line " << __LINE__ << "\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

using progressive::desktop::SasSession;
using progressive::desktop::sasCreate;
using progressive::desktop::sasSetTheirKey;
using progressive::desktop::sasGenerateBytes;
using progressive::desktop::sasCalculateMac;
using progressive::desktop::sasVerifyMac;
using progressive::desktop::computeSasEmojis;
using progressive::desktop::computeSasDecimals;

static bool isBase64(const std::string& s) {
    static const char* CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (char c : s) {
        if (c == '=') continue;
        if (std::strchr(CHARS, c) == nullptr) return false;
    }
    return true;
}

// Bug 2 regression guard: pubkey is single-base64 (44 chars, valid), NOT double-encoded.
static void test_pubkey_base64() {
    SasSession alice = sasCreate();
    CHECK(alice.valid, "sasCreate produces valid session");
    CHECK_EQ((int)alice.ourPubkey.size(), 43, "pubkey is 43 chars (unpadded base64, 32-byte curve25519)");
    CHECK(isBase64(alice.ourPubkey), "pubkey is valid base64 (no double-encode)");
}

// Core roundtrip: two sessions exchange pubkeys, derive shared bytes, emojis, decimals, MACs.
static void test_sas_roundtrip() {
    SasSession alice = sasCreate();
    SasSession bob = sasCreate();
    CHECK(alice.valid && bob.valid, "both SAS sessions valid");

    // Exchange pubkeys (each sets the other's pubkey as their key)
    CHECK(sasSetTheirKey(bob, alice.ourPubkey), "bob sets alice's pubkey");
    CHECK(sasSetTheirKey(alice, bob.ourPubkey), "alice sets bob's pubkey");
    CHECK(alice.theirKeySet && bob.theirKeySet, "both have their key set");

    // Both generate SAS bytes with the same info string -> must be identical
    std::string info = "MATRIX_KEY_VERIFICATION_SAS|alice|devA|keyA|bob|devB|keyB|txn1";
    std::string aliceBytes = sasGenerateBytes(alice, info);
    std::string bobBytes = sasGenerateBytes(bob, info);
    CHECK(!aliceBytes.empty(), "alice SAS bytes non-empty");
    CHECK_EQ((int)aliceBytes.size(), 6, "alice SAS bytes is 6 bytes");
    CHECK(aliceBytes == bobBytes, "alice and bob SAS bytes match (shared secret agrees)");

    // Emojis: 7 emojis, identical on both sides
    auto aliceEmojis = computeSasEmojis(aliceBytes);
    auto bobEmojis = computeSasEmojis(bobBytes);
    CHECK_EQ((int)aliceEmojis.size(), 7, "alice produces 7 emojis");
    CHECK_EQ((int)bobEmojis.size(), 7, "bob produces 7 emojis");
    CHECK(aliceEmojis.size() == bobEmojis.size(), "emoji count matches");
    bool emojisMatch = true;
    for (size_t i = 0; i < aliceEmojis.size(); i++) {
        if (aliceEmojis[i].emoji != bobEmojis[i].emoji) { emojisMatch = false; break; }
    }
    CHECK(emojisMatch, "alice and bob emojis identical");

    // Decimals: 3 numbers, identical, each in [1000, 9191]
    auto aliceDec = computeSasDecimals(aliceBytes);
    auto bobDec = computeSasDecimals(bobBytes);
    CHECK_EQ((int)aliceDec.size(), 3, "alice produces 3 decimals");
    CHECK_EQ((int)bobDec.size(), 3, "bob produces 3 decimals");
    CHECK(aliceDec == bobDec, "alice and bob decimals identical");
    bool inRange = true;
    for (int d : aliceDec) {
        if (d < 1000 || d > 9191) { inRange = false; break; }
    }
    CHECK(inRange, "all decimals in range [1000, 9191]");
}

// Bug 1 regression guard: MAC compute/verify roundtrip (ret=0, no resize crash).
static void test_mac_roundtrip() {
    SasSession alice = sasCreate();
    SasSession bob = sasCreate();
    sasSetTheirKey(bob, alice.ourPubkey);
    sasSetTheirKey(alice, bob.ourPubkey);

    std::string info = "MATRIX_KEY_VERIFICATION_MAC|alice|devA|bob|devB|txn1|ed25519:devA";
    std::string msg = "alice-ed25519-key-base64";
    std::string mac = sasCalculateMac(alice, msg, info);
    CHECK(!mac.empty(), "sasCalculateMac returns non-empty (Bug 1: no resize crash)");
    CHECK_EQ((int)mac.size(), 43, "MAC is 43 chars (unpadded base64, 32-byte SHA256)");

    // Bob verifies alice's MAC
    CHECK(sasVerifyMac(bob, mac, msg, info), "bob verifies alice's MAC");

    // Reverse: bob computes, alice verifies
    std::string mac2 = sasCalculateMac(bob, msg, info);
    CHECK(sasVerifyMac(alice, mac2, msg, info), "alice verifies bob's MAC");

    // Mismatched message -> verification fails
    CHECK(!sasVerifyMac(bob, mac, "wrong-message", info), "mismatched message fails MAC verification");
}

int main() {
    test_pubkey_base64();
    test_sas_roundtrip();
    test_mac_roundtrip();
    if (failures > 0) {
        std::cerr << "\n" << failures << " test(s) FAILED\n";
        return 1;
    }
    std::cout << "\nAll SAS crypto roundtrip tests passed\n";
    return 0;
}

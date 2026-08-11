// Smoke test for the vendored ecore (progressive-desktop E2EE core).
// Exercises self-contained crypto pieces that need no MatrixClient/SessionStore:
// OlmAccountStore (create/sign/pickle), ed25519Verify, SAS emoji computation.
//
// NOTE: never put setup calls inside assert() — Release builds define NDEBUG
// and assert() compiles to a no-op, silently skipping the call (this exact
// bug produced all-zero keys in the first version of this test).
#include <cassert>
#include <iostream>
#include <string>

#include "core/crypto/olm_account.hpp"
#include "core/crypto/ed25519.hpp"
#include "core/crypto/sas.hpp"
#include "core/crypto/sas_emojis.hpp"

using namespace progressive::desktop;

static void test_olm_account() {
    OlmAccountStore acc;
    bool created = acc.create();  // explicit call — NOT inside assert()
    assert(created && "account create");
    assert(!acc.curve25519Key().empty() && "curve25519 identity key");
    assert(!acc.ed25519Key().empty() && "ed25519 fingerprint key");

    // Keys must be non-degenerate: base64 of 32 zero bytes is 43 'A's.
    assert(acc.curve25519Key() != std::string(43, 'A') && "identity key is not zeroed");
    assert(acc.ed25519Key() != std::string(43, 'A') && "fingerprint key is not zeroed");

    // Sign + verify roundtrip
    std::string msg = "{\"test\":\"canonical-json\"}";
    std::string sig = acc.sign(msg);
    assert(!sig.empty() && "signature produced");
    assert(ed25519Verify(acc.ed25519Key(), msg, sig) && "signature verifies");
    assert(!ed25519Verify(acc.ed25519Key(), msg + "x", sig) && "tampered message rejected");

    // One-time keys + pickle roundtrip
    std::string otk = acc.generateOneTimeKeys(5);
    assert(!otk.empty() && "one-time keys generated");
    std::string pickle = acc.save("test-key");
    assert(!pickle.empty() && "account pickle saved");

    OlmAccountStore loaded;
    bool loaded_ok = loaded.load(pickle, "test-key");  // explicit call
    assert(loaded_ok && "account pickle loaded");
    assert(loaded.curve25519Key() == acc.curve25519Key() && "identity key survives pickle");
    assert(loaded.sign(msg) == sig && "signature stable across pickle reload");
    std::cout << "  olm_account: OK (curve25519=" << acc.curve25519Key().substr(0, 16) << "...)\n";
}

static void test_sas() {
    std::string sasBytes(32, '\x42');  // deterministic bytes
    auto emojis = computeSasEmojis(sasBytes);
    assert(emojis.size() == 7 && "SAS produces 7 emojis");
    auto decimals = computeSasDecimals(sasBytes);
    assert(decimals.size() == 3 && "SAS produces 3 decimals");
    assert(!formatSasEmojis(emojis).empty() && "emoji formatting works");
    assert(!formatSasDecimals(decimals).empty() && "decimal formatting works");

    // Two sessions with the same shared secret must produce identical emoji
    // sets (current core API: sessions are created and keys exchanged first).
    std::string secret = "shared-secret-bytes";
    SasSession a = sasCreate();
    SasSession b = sasCreate();
    assert(a.valid && b.valid && "SAS sessions created");
    assert(sasSetTheirKey(a, b.ourPubkey) && "A accepts B's key");
    assert(sasSetTheirKey(b, a.ourPubkey) && "B accepts A's key");
    auto bytesA = sasGenerateBytes(a, secret);
    auto bytesB = sasGenerateBytes(b, secret);
    assert(!bytesA.empty() && "SAS bytes generated");
    assert(computeSasEmojis(bytesA) == computeSasEmojis(bytesB) && "SAS emojis match");
    std::cout << "  sas: OK (" << emojis[0].emoji << " " << emojis[0].description << ", ...)\n";
}

int main() {
    std::cout << "ecore smoke tests:\n";
    test_olm_account();
    test_sas();
    std::cout << "ALL ECORE SMOKE TESTS PASSED\n";
    return 0;
}

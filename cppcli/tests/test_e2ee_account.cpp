#include "core/crypto/olm_account.hpp"
#include <progressive/olm.hpp>
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

static void test_shared_flag_lifecycle() {
    progressive::desktop::OlmAccountStore store;
    CHECK(store.create(), "create() succeeds");
    CHECK(!store.shared(), "shared() false after create");
    store.markAsShared();
    CHECK(store.shared(), "shared() true after markAsShared");
    store.setShared(false);
    CHECK(!store.shared(), "shared() false after setShared(false)");
    store.setShared(true);
    CHECK(store.shared(), "shared() true after setShared(true)");
}

static void test_key_lifecycle() {
    progressive::desktop::OlmAccountStore store;
    CHECK(store.create(), "create() succeeds");
    std::string curve = store.curve25519Key();
    std::string ed = store.ed25519Key();
    CHECK_EQ((int)curve.size(), 43, "curve25519 key is 43 chars");
    CHECK_EQ((int)ed.size(), 43, "ed25519 key is 43 chars");
    std::string otks = store.generateOneTimeKeys(5);
    CHECK(!otks.empty(), "generateOneTimeKeys(5) non-empty");
    CHECK(otks.find("curve25519") != std::string::npos, "OTK JSON contains curve25519");
    store.markOneTimeKeysPublished();
    std::string otks2 = store.generateOneTimeKeys(5);
    CHECK(!otks2.empty(), "generateOneTimeKeys after publish non-empty");
    CHECK(otks2.find("curve25519") != std::string::npos, "OTK2 JSON contains curve25519");
}

static void test_count_tracking() {
    progressive::desktop::OlmAccountStore store;
    CHECK(store.create(), "create() succeeds");
    CHECK_EQ(store.uploadedKeyCount(), 0, "uploadedKeyCount() 0 after create");
    store.setUploadedKeyCount(42);
    CHECK_EQ(store.uploadedKeyCount(), 42, "uploadedKeyCount() 42 after set");
    store.setUploadedKeyCount(100);
    CHECK_EQ(store.uploadedKeyCount(), 100, "uploadedKeyCount() 100 after set");
}

static void test_save_load_roundtrip() {
    progressive::desktop::OlmAccountStore store1;
    CHECK(store1.create(), "store1 create()");
    store1.markAsShared();
    store1.setUploadedKeyCount(77);
    std::string curveKey = store1.curve25519Key();
    std::string edKey = store1.ed25519Key();

    std::string pickle = store1.save("roundtrip-key");
    CHECK(!pickle.empty(), "save() returns non-empty pickle");

    progressive::desktop::OlmAccountStore store2;
    CHECK(store2.load(pickle, "roundtrip-key"), "store2 load() succeeds");
    CHECK_EQ(store2.curve25519Key(), curveKey, "curve25519 matches after load");
    CHECK_EQ(store2.ed25519Key(), edKey, "ed25519 matches after load");
    // shared and uploadedKeyCount are NOT in the libolm pickle —
    // they are stored in the SQLite olm_account table (tested via session_store DB layer).
    // Round-trip of identity keys is the important persistence invariant.
}

int main() {
    test_shared_flag_lifecycle();
    test_key_lifecycle();
    test_count_tracking();
    test_save_load_roundtrip();
    if (failures == 0) { std::cout << "\nALL TESTS PASSED\n"; return 0; }
    std::cout << "\n" << failures << " TEST(S) FAILED\n";
    return 1;
}

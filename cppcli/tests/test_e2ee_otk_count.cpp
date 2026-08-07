#include "core/crypto/olm_account.hpp"
#include "core/crypto/decryptor.hpp"
#include "core/crypto/sig_verify.hpp"
#include "core/fast_sync.hpp"
#include <simdjson.h>
#include <iostream>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)
#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (expected " << (b) << " got " << (a) << ") line " << __LINE__ << "\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

static int countOTKs(const std::string& json) {
    simdjson::dom::parser parser;
    auto doc = parser.parse(json);
    if (doc.error() != simdjson::SUCCESS) return -1;
    auto obj = doc.value().get_object();
    if (obj.error() != simdjson::SUCCESS) return -1;
    int count = 0;
    for (auto field : obj.value()) {
        auto inner = field.value.get_object();
        if (inner.error() == simdjson::SUCCESS) {
            for (auto f : inner.value()) count++;
        } else {
            count++;
        }
    }
    return count;
}

static void test_otk_count_lifecycle() {
    progressive::desktop::OlmAccountStore store;
    CHECK(store.create(), "create() succeeds");
    CHECK_EQ(store.uploadedKeyCount(), 0, "uploadedKeyCount 0 after create");

    std::string otks = store.generateOneTimeKeys(10);
    int n = countOTKs(otks);
    CHECK_EQ(n, 10, "generateOneTimeKeys(10) produces 10 keys");
    CHECK(otks.find("curve25519") != std::string::npos, "OTK JSON contains curve25519");

    store.markOneTimeKeysPublished();
    std::string otks2 = store.generateOneTimeKeys(10);
    int n2 = countOTKs(otks2);
    CHECK_EQ(n2, 10, "after publish, generateOneTimeKeys(10) produces 10 keys");

    store.markOneTimeKeysPublished();
    store.setUploadedKeyCount(100);
    CHECK_EQ(store.uploadedKeyCount(), 100, "setUploadedKeyCount(100) sticks");
    std::string otks3 = store.generateOneTimeKeys(10);
    CHECK(!otks3.empty(), "generateOneTimeKeys still callable when count=100 (no crash)");
}

static void test_count_roundtrip() {
    progressive::desktop::OlmAccountStore store1;
    CHECK(store1.create(), "store1 create()");
    store1.generateOneTimeKeys(5);
    store1.markOneTimeKeysPublished();
    store1.setUploadedKeyCount(42);

    std::string pickle = store1.save("otk-roundtrip-key");
    CHECK(!pickle.empty(), "save() non-empty");

    progressive::desktop::OlmAccountStore store2;
    CHECK(store2.load(pickle, "otk-roundtrip-key"), "load() succeeds");
    // uploadedKeyCount is NOT in libolm pickle — DB-layer, verified in test_e2ee_account
}

static void test_fallback_key_lifecycle() {
    progressive::desktop::OlmAccountStore store;
    CHECK(store.create(), "fallback: create() succeeds");

    CHECK(store.unpublishedFallbackKey().empty(), "fallback: no unpublished key before generate");
    CHECK(store.generateFallbackKey(), "fallback: generateFallbackKey() succeeds");
    std::string fk = store.unpublishedFallbackKey();
    CHECK(!fk.empty(), "fallback: unpublishedFallbackKey() non-empty after generate");
    CHECK(fk.find("curve25519") != std::string::npos, "fallback: JSON contains curve25519");

    store.markOneTimeKeysPublished();
    CHECK(store.unpublishedFallbackKey().empty(),
          "fallback: unpublished empty after markOneTimeKeysPublished (marks fallback too)");

    CHECK(store.generateFallbackKey(), "fallback: regenerate after publish works");
    CHECK(!store.unpublishedFallbackKey().empty(), "fallback: new unpublished key exists");
}

static void test_fallback_key_roundtrip() {
    progressive::desktop::OlmAccountStore store1;
    CHECK(store1.create(), "fallback-roundtrip: create()");
    CHECK(store1.generateFallbackKey(), "fallback-roundtrip: generate()");
    CHECK(!store1.unpublishedFallbackKey().empty(), "fallback-roundtrip: unpublished before save");

    std::string pickle = store1.save("fallback-roundtrip-key");
    CHECK(!pickle.empty(), "fallback-roundtrip: save() non-empty");

    progressive::desktop::OlmAccountStore store2;
    CHECK(store2.load(pickle, "fallback-roundtrip-key"), "fallback-roundtrip: load() succeeds");
    // The fallback key lives inside the libolm account pickle — it survives
    // save/load without any DB schema change.
    CHECK(!store2.unpublishedFallbackKey().empty(),
          "fallback-roundtrip: unpublished fallback survives save/load");
}

static void test_sync_unused_fallback_parse() {
    // device_unused_fallback_key_types present → parsed into the response.
    std::string err;
    auto resp = progressive::desktop::parseSyncResponseFast(
        R"({"device_unused_fallback_key_types":["signed_curve25519","other"],
            "device_one_time_keys_count":{"signed_curve25519":5}})",
        err);
    CHECK(err.empty(), "sync-fallback: parse succeeds");
    CHECK(resp.unusedFallbackKeyTypes.size() == 2,
          "sync-fallback: two fallback types parsed");
    CHECK(resp.unusedFallbackKeyTypes[0] == "signed_curve25519",
          "sync-fallback: first type is signed_curve25519");

    // Field absent → empty vector (the "never uploaded" case → trigger upload).
    auto resp2 = progressive::desktop::parseSyncResponseFast(
        R"({"device_one_time_keys_count":{"signed_curve25519":5}})", err);
    CHECK(resp2.unusedFallbackKeyTypes.empty(),
          "sync-fallback: absent field → empty vector");
}

static void test_rng_is_csprng() {
    // Two fresh accounts must get different identity keys (depends on random bytes,
    // not sequential counters — catches unseeded-rand() regression).
    progressive::desktop::OlmAccountStore a, b;
    CHECK(a.create(), "rng: first account created");
    CHECK(b.create(), "rng: second account created");
    CHECK(a.ed25519Key() != b.ed25519Key(), "rng: two accounts have different ed25519 keys");
    CHECK(a.curve25519Key() != b.curve25519Key(), "rng: two accounts have different curve25519 keys");
}


static void test_signed_fallback_body() {
    progressive::desktop::Decryptor dec;
    CHECK(dec.init(), "fallback-body: decryptor init");
    std::string userId = "t";
    std::string deviceId = "d";

    CHECK(dec.account()->generateFallbackKey(), "fallback-body: generate");
    std::string raw = dec.account()->unpublishedFallbackKey();
    CHECK(!raw.empty(), "fallback-body: unpublished non-empty");

    std::string section = dec.buildFallbackKeysSection(userId, deviceId);
    CHECK(!section.empty(), "fallback-body: section non-empty");

    simdjson::dom::parser p;
    auto doc = p.parse(section);
    CHECK(doc.error() == simdjson::SUCCESS, "fallback-body: valid JSON");

    auto root = doc.value().get_object();
    CHECK(root.error() == simdjson::SUCCESS, "fallback-body: root object");
    bool found = false;
    for (auto field : root.value()) {
        std::string key(field.key);
        if (key.find("signed_curve25519:") != 0) continue;
        auto val = field.value.get_object();
        if (val.error() != simdjson::SUCCESS) continue;
        auto kf = val.value()["key"].get_string();
        CHECK(kf.error() == simdjson::SUCCESS, "fallback-body: key field");
        auto sf = val.value()["signatures"][userId]["ed25519:" + deviceId].get_string();
        CHECK(sf.error() == simdjson::SUCCESS, "fallback-body: signature found");
        std::string ed = dec.account()->ed25519Key();
        CHECK(progressive::desktop::verifyOtk(ed, std::string(kf.value()), std::string(sf.value())),
              "fallback-body: verifyOtk returns true");
        found = true;
    }
    CHECK(found, "fallback-body: signed_curve25519 key found");

    // After publish: buildFallbackKeysSection generates a fresh key (new behavior)
    // and the new section must be non-empty with a different key value.
    dec.markOneTimeKeysPublished();
    std::string section2 = dec.buildFallbackKeysSection(userId, deviceId);
    CHECK(!section2.empty(), "fallback-body: section non-empty after publish (generates fresh)");
    CHECK(section2 != section, "fallback-body: fresh key differs from original");
}

int main() {
    test_otk_count_lifecycle();
    test_count_roundtrip();
    test_fallback_key_lifecycle();
    test_fallback_key_roundtrip();
    test_sync_unused_fallback_parse();
    test_rng_is_csprng();
    test_signed_fallback_body();
    if (failures == 0) { std::cout << "\nALL TESTS PASSED\n"; return 0; }
    std::cout << "\n" << failures << " TEST(S) FAILED\n";
    return 1;
}

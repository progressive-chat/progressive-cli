#include "core/crypto/megolm_store.hpp"
#include "core/crypto/decryptor.hpp"
#include "core/crypto/cross_sign.hpp"
#include "core/crypto/sig_verify.hpp"
#include "core/crypto/recovery_key.hpp"
#include "core/crypto/backup_crypto.hpp"
#include "core/crypto/ssss.hpp"
#include "core/session_store.hpp"
#include <iostream>
#include <simdjson.h>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)
#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (expected " << (b) << " got " << (a) << ") line " << __LINE__ << "\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

static void test_megolm_empty_pickle() {
    progressive::desktop::MegolmStore store;
    std::string pkl = store.pickleAll("empty-key");
    CHECK(pkl == "[]" || pkl.empty(),
        "pickleAll on empty store returns [] or empty");
    CHECK(store.unpickleAll("empty-key", ""),
        "unpickleAll(\"\") returns true (no-op)");
    CHECK(store.unpickleAll("empty-key", "[]"),
        "unpickleAll(\"[]\") returns true (empty array)");
}

static void test_megolm_garbage_unpickle() {
    progressive::desktop::MegolmStore store;
    bool ok = store.unpickleAll("garbage-key", "not valid json at all");
    CHECK(!ok, "unpickleAll with garbage data returns false");
}

static void test_stale_device_cap() {
    progressive::desktop::Decryptor dec;
    dec.init();

    // Insert 1000 entries — should all be stale
    for (int i = 0; i < 1000; i++) {
        dec.markDevicesStale({"@user" + std::to_string(i) + ":matrix.org"});
    }
    CHECK(dec.isDeviceStale("@user999:matrix.org"),
        "user 999 is stale after 1000 inserts");
    CHECK(dec.isDeviceStale("@user0:matrix.org"),
        "user 0 is stale after cap not exceeded");

    // Insert beyond cap — 1001st should NOT be added
    dec.markDevicesStale({"@beyond:matrix.org"});
    CHECK(!dec.isDeviceStale("@beyond:matrix.org"),
        "user beyond cap NOT stale (cap at 1000)");

    // clearStale works
    dec.clearStale("@user0:matrix.org");
    CHECK(!dec.isDeviceStale("@user0:matrix.org"),
        "clearStale removes stale flag");
}


// Export/import roundtrip: outbound session appears in the envelope; a
// hand-built envelope imports into the store.
static void test_key_export_import() {
    progressive::desktop::Decryptor dec;
    CHECK(dec.init(), "xport: decryptor init");

    // Create an outbound session -> the self-echo inbound appears in the export.
    std::string sessId = dec.getOrCreateOutboundSession("!room1:test");
    CHECK(!sessId.empty(), "xport: outbound session created");
    std::string envelope = dec.exportAllKeys();
    CHECK(!envelope.empty(), "xport: export envelope non-empty");
    CHECK(envelope.find("\"version\":1") != std::string::npos, "xport: version 1");
    CHECK(envelope.find("!room1:test") != std::string::npos, "xport: room in envelope");
    // No duplicate room keys (the outbound merge would have doubled this room).
    size_t roomEntries = 0;
    std::string needle = "\"!room1:test\":{\"sessions\"";
    for (size_t p = envelope.find(needle); p != std::string::npos;
         p = envelope.find(needle, p + 1)) roomEntries++;
    CHECK(roomEntries == 1, "xport: exactly one rooms entry for the room (no duplicate)");

    // Encrypt a real message with the outbound session.
    std::string encContent = dec.encryptMessage("!room1:test", "DEV",
        "{\"msg\":\"hello-xport\"}");
    CHECK(!encContent.empty(), "xport: encrypted a message");

    // Full roundtrip: import into a second decryptor and DECRYPT the message.
    progressive::desktop::Decryptor dec2;
    CHECK(dec2.init(), "xport: decryptor2 init");
    int n = dec2.importKeys(envelope);
    CHECK(n > 0, "xport: import returns count > 0");
    auto dec2res = dec2.decryptMegolmEvent("!room1:test", "@alice:test",
        encContent, "eid1", 0);
    CHECK(dec2res.ok && dec2res.plaintext.find("hello-xport") != std::string::npos,
          "xport: second decryptor decrypts the exported session's message");
    (void)n;
}


// Rotation: with rotation_period_msgs=1, sending a message then re-requesting
// the outbound session must yield a NEW session id.
static void test_megolm_rotation() {
    progressive::desktop::Decryptor dec;
    CHECK(dec.init(), "rot: init");
    std::string s1 = dec.getOrCreateOutboundSession("!r:test");
    CHECK(!s1.empty(), "rot: session created");

    dec.setRoomEncryptionConfig("!r:test",
        "{\"algorithm\":\"m.megolm.v1.aes-sha2\",\"rotation_period_msgs\":1}");

    std::string enc = dec.encryptMessage("!r:test", "DEV", "{}");
    CHECK(!enc.empty(), "rot: encrypted");

    std::string s2 = dec.getOrCreateOutboundSession("!r:test");
    CHECK(s2 != s1, "rot: session rotated after message count reached");

    // Without a config, no rotation.
    std::string s3 = dec.getOrCreateOutboundSession("!r:test");
    CHECK(s3 == s2, "rot: no rotation without config");
}


// Cross-signing: keygen, sign/verify roundtrip, tamper rejection, builders.
static void test_cross_signing() {
    auto keys = progressive::desktop::generateCrossSigningKeys();
    CHECK(!keys.masterPub.empty() && !keys.selfPriv.empty(), "xs: keys generated");
    CHECK(keys.selfPub != keys.masterPub && keys.selfPub != keys.userPub,
          "xs: distinct keypairs");

    std::string msg = "hello-cross-sign";
    std::string sig = progressive::desktop::signEd25519(keys.masterPriv, msg);
    CHECK(!sig.empty(), "xs: signature produced");
    CHECK(progressive::desktop::verifyEd25519(keys.masterPub, msg, sig),
          "xs: signature verifies");
    CHECK(!progressive::desktop::verifyEd25519(keys.masterPub, msg + "x", sig),
          "xs: tampered message rejected");
    CHECK(!progressive::desktop::verifyEd25519(keys.selfPub, msg, sig),
          "xs: wrong key rejected");

    std::string content = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.self_signing", keys.selfPub,
        keys.masterPub, keys.masterPriv, "@alice:test");
    CHECK(content.find("\"signatures\"") != std::string::npos
          && content.find("@alice:test") != std::string::npos,
          "xs: content has signatures map with our user");
    std::string canonical = progressive::desktop::crossSigningKeysCanonical(keys.selfPub);
    CHECK(canonical.find("ed25519:" + keys.selfPub) != std::string::npos,
          "xs: canonical keys object");

    // Extract the signature from the content and verify it with the master key —
    // proves the account-data trust chain is real, not just present.
    std::string contentSig;
    std::string marker = "ed25519:" + keys.masterPub + "\":\"";
    auto sigPos = content.find(marker);
    if (sigPos != std::string::npos) {
        sigPos += marker.size();
        auto sigEnd = content.find('"', sigPos);
        if (sigEnd != std::string::npos) contentSig = content.substr(sigPos, sigEnd - sigPos);
    }
    CHECK(!contentSig.empty(), "xs: extracted content signature");
    // The signature is over the FULL canonical CrossSigningKey (keys + usage + user_id).
    std::string fullCanonical = progressive::desktop::crossSigningKeyCanonical(
        keys.selfPub, "self_signing", "@alice:test");
    CHECK(progressive::desktop::verifyEd25519(keys.masterPub, fullCanonical, contentSig),
          "xs: content signature verifies with the master key");
    // And it must NOT verify over the keys-only canonical (regression guard).
    CHECK(!progressive::desktop::verifyEd25519(keys.masterPub, canonical, contentSig),
          "xs: keys-only canonical does NOT verify (full canonical required)");
}

// Trust computation: SSK-signed device -> Trusted, unsigned -> Unverified,
// no cross-signing published -> empty result.
static void test_trust_computation() {
    using namespace progressive::desktop;
    auto keys = generateCrossSigningKeys();

    // A device whose keys are SSK-signed (canonical device_keys).
    std::string devId = "DEVT1";
    std::string curve = "curveDevT1";
    std::string ed = "edDevT1";
    std::string canonical = buildDeviceKeysCanonical("@alice:test", devId, curve, ed);
    std::string sskSig = signEd25519(keys.selfPriv, canonical);

    std::string queryJson =
        "{\"device_keys\":{\"@alice:test\":{"
        "\"" + devId + "\":{\"user_id\":\"@alice:test\",\"device_id\":\"" + devId + "\","
        "\"algorithms\":[\"m.olm.v1.curve25519-aes-sha2\"],"
        "\"keys\":{\"curve25519:" + devId + "\":\"" + curve + "\","
        "\"ed25519:" + devId + "\":\"" + ed + "\"},"
        "\"signatures\":{\"@alice:test\":{\"ed25519:" + keys.selfPub + "\":\"" + sskSig + "\"}}}"
        "}},\"master_keys\":{\"@alice:test\":{\"keys\":{\"ed25519:" + keys.masterPub + "\":\"" + keys.masterPub + "\"}}},"
        "\"self_signing_keys\":{\"@alice:test\":{\"keys\":{\"ed25519:" + keys.selfPub + "\":\"" + keys.selfPub + "\"}}}}";

    auto trust = computeDeviceTrust(queryJson, "@alice:test");
    CHECK(trust.size() == 1, "trust: one device evaluated");
    if (!trust.empty()) {
        CHECK(trust[0].deviceId == devId, "trust: device id");
        CHECK(trust[0].trust == DeviceTrust::Trusted, "trust: SSK-signed device is Trusted");
    }

    // Tamper the signature (a data char, NOT the base64 padding — the lenient
    // decoder ignores trailing padding) -> Unverified.
    std::string badSig = sskSig;
    if (badSig.size() > 10) {
        badSig[10] = (badSig[10] == 'A') ? 'B' : 'A';
    }
    std::string badQuery = queryJson;
    auto pos = badQuery.find(sskSig);
    if (pos != std::string::npos) badQuery.replace(pos, sskSig.size(), badSig);
    auto trust2 = computeDeviceTrust(badQuery, "@alice:test");
    CHECK(trust2.size() == 1 && trust2[0].trust == DeviceTrust::Unverified,
          "trust: tampered SSK signature -> Unverified");

    // No self_signing_keys -> empty (user has no cross-signing).
    std::string noXs = "{\"device_keys\":{},\"master_keys\":{},\"self_signing_keys\":{}}";
    CHECK(computeDeviceTrust(noXs, "@alice:test").empty(), "trust: no cross-signing -> empty");

    // Cross-user: bob's master key signed by OUR USK -> all bob's devices Verified.
    auto usk = generateCrossSigningKeys();
    std::string bobMaster = "bobMasterPub";
    std::string bobMasterSig = signEd25519(usk.userPriv,
        crossSigningKeyCanonical(bobMaster, "master", "@bob:test"));
    std::string crossJson =
        "{\"device_keys\":{\"@bob:test\":{"
        "\"DEVB\":{\"user_id\":\"@bob:test\",\"device_id\":\"DEVB\","
        "\"algorithms\":[\"m.olm.v1.curve25519-aes-sha2\"],"
        "\"keys\":{\"curve25519:DEVB\":\"cB\",\"ed25519:DEVB\":\"eB\"},"
        "\"signatures\":{\"@bob:test\":{\"ed25519:sbPub\":\"sigB\"}}}"
        "}},\"master_keys\":{\"@bob:test\":{\"keys\":{\"ed25519:" + bobMaster + "\":\"" + bobMaster + "\"},"
        "\"signatures\":{\"@alice:test\":{\"ed25519:" + usk.userPub + "\":\"" + bobMasterSig + "\"}}}},"
        "\"self_signing_keys\":{\"@bob:test\":{\"keys\":{\"ed25519:sbPub\":\"sbPub\"}}}}";
    auto trustCross = computeDeviceTrust(crossJson, "@bob:test", "@alice:test", usk.userPub);
    CHECK(trustCross.size() == 1 && trustCross[0].trust == DeviceTrust::Verified,
          "trust: USK-signed master upgrades bob's device to Verified");
    // Without our USK params the same data stays Unverified (the SSK sig is bogus).
    auto trustNoUsk = computeDeviceTrust(crossJson, "@bob:test");
    CHECK(trustNoUsk.size() == 1 && trustNoUsk[0].trust == DeviceTrust::Unverified,
          "trust: without our USK the device stays Unverified");
}

// Verified-devices lifecycle: save -> clear -> gone.
static void test_verified_devices_clear() {
    progressive::desktop::SessionStore store;
    if (!store.open("/tmp/pd_test_verified.db")) { CHECK(false, "vd: open store"); return; }
    CHECK(store.saveVerifiedDevice("@alice:test", "DEVA"), "vd: save verified device");
    CHECK(store.isDeviceVerified("@alice:test", "DEVA"), "vd: device verified");
    CHECK(!store.isDeviceVerified("@bob:test", "DEVB"), "vd: other device not verified");
    CHECK(store.clearVerifiedDevices(), "vd: clear all");
    CHECK(!store.isDeviceVerified("@alice:test", "DEVA"), "vd: cleared device no longer verified");
    store.close();
}

// Phase 7: recovery key + backup crypto roundtrip.
static void test_key_backup_crypto() {
    using namespace progressive::desktop;

    // base58 roundtrip.
    std::vector<uint8_t> bytes = {0x00, 0x01, 0x02, 0xFE, 0xFF};
    CHECK(base58Decode(base58Encode(bytes)) == bytes, "bk: base58 roundtrip");

    // Recovery key: valid + parity-protected.
    std::string rk = generateRecoveryKey();
    CHECK(!rk.empty(), "bk: recovery key generated");
    CHECK(isValidRecoveryKey(rk), "bk: recovery key valid");
    std::string bad = rk;
    if (bad.size() > 3) {
        bad[bad.size() / 2] = (bad[bad.size() / 2] == 'A') ? 'B' : 'A';
        CHECK(!isValidRecoveryKey(bad), "bk: tampered recovery key rejected");
    }

    // Backup keypair derivation from the seed.
    auto seed = recoveryKeySeed(rk);
    auto pair = deriveBackupKey(seed);
    CHECK(!pair.publicKeyB64.empty() && !pair.privateKeyB64.empty(),
          "bk: backup keypair derived");

    // Encrypt a payload (a megolm export is opaque to the backup crypto) ->
    // decrypt with the private key -> identical.
    std::string exportB64 = "AgAAAABQb2bFnRxmdVHpOjJpZ5y0NTVhTkZVRnBha1NlY3JldEV4cG9ydERhdGE=";
    std::string sd = encryptBackupSessionData(exportB64, pair.publicKeyB64);
    CHECK(!sd.empty(), "bk: session_data encrypted");
    std::string restored = decryptBackupSessionData(sd, pair.privateKeyB64);
    CHECK(restored == exportB64, "bk: session_data roundtrip");

    // Structure builders.
    BackupVersionInfo info;
    info.algorithm = "m.megolm_backup.v1.curve25519-aes-sha2";
    info.publicKey = pair.publicKeyB64;
    std::string body = buildBackupVersionBody(info);
    CHECK(body.find("\"algorithm\":\"m.megolm_backup.v1.curve25519-aes-sha2\"") != std::string::npos
          && body.find(pair.publicKeyB64) != std::string::npos,
          "bk: version body structure");
    std::string entry = buildBackupSessionEntry(sd, 0);
    CHECK(entry.find("\"first_message_index\":0") != std::string::npos
          && entry.find(sd) != std::string::npos,
          "bk: session entry structure");
}

// Phase 7: REAL megolm export -> backup encrypt -> restore decrypt -> import
// into a second decryptor -> decrypt a real message. Plus the store registry.
static void test_key_backup_full_roundtrip() {
    using namespace progressive::desktop;

    // Sender decryptor with a real session + message.
    Decryptor sender;
    CHECK(sender.init(), "bkrt: sender init");
    std::string sessId = sender.getOrCreateOutboundSession("!bk:test");
    CHECK(!sessId.empty(), "bkrt: outbound session created");
    std::string encContent = sender.encryptMessage("!bk:test", "DEV",
        "{\"msg\":\"backup-roundtrip\"}");
    CHECK(!encContent.empty(), "bkrt: encrypted a message");
    std::string envelope = sender.exportAllKeys();
    CHECK(envelope.find("!bk:test") != std::string::npos, "bkrt: envelope has the room");

    // Extract the first session entry (room_id, session_id, sender_key, session_key).
    std::string roomId, sessionId, senderKey, sessionKey;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(envelope);
        if (doc.error() == simdjson::SUCCESS) {
            auto rooms = doc.value()["rooms"].get_object();
            if (rooms.error() == simdjson::SUCCESS) {
                for (auto room : rooms.value()) {
                    roomId = std::string(room.key);
                    auto sessions = room.value["sessions"].get_array();
                    if (sessions.error() != simdjson::SUCCESS) continue;
                    for (auto sess : sessions.value()) {
                        auto sid = sess["session_id"].get_string();
                        auto sk = sess["session_key"].get_string();
                        auto skey = sess["sender_key"].get_string();
                        if (sid.error() == simdjson::SUCCESS) sessionId = std::string(sid.value());
                        if (sk.error() == simdjson::SUCCESS) sessionKey = std::string(sk.value());
                        if (skey.error() == simdjson::SUCCESS) senderKey = std::string(skey.value());
                    }
                }
            }
        }
    }
    CHECK(!roomId.empty() && !sessionId.empty() && !sessionKey.empty(),
          "bkrt: parsed session fields");
    CHECK(!senderKey.empty(), "bkrt: parsed sender_key");

    // Backup encrypt -> restore decrypt -> the same raw export.
    std::string rk = generateRecoveryKey();
    auto pair = deriveBackupKey(recoveryKeySeed(rk));
    std::string sd = encryptBackupSessionData(sessionKey, pair.publicKeyB64);
    CHECK(!sd.empty(), "bkrt: real export encrypted");
    std::string restored = decryptBackupSessionData(sd, pair.privateKeyB64);
    CHECK(restored == sessionKey, "bkrt: real export restored");

    // Import into a second decryptor and decrypt the real message.
    Decryptor receiver;
    CHECK(receiver.init(), "bkrt: receiver init");
    std::string realId = receiver.importSingleSession(roomId, senderKey, restored);
    CHECK(!realId.empty(), "bkrt: imported the restored session");
    auto res = receiver.decryptMegolmEvent(roomId, "@alice:test", encContent, "eidB", 0);
    CHECK(res.ok && res.plaintext.find("backup-roundtrip") != std::string::npos,
          "bkrt: receiver decrypts the backed-up session's message");

    // Store registry roundtrip.
    SessionStore store;
    CHECK(store.open("/tmp/pd_test_backup.db"), "bkrt: store open");
    BackupInfo bi;
    bi.version = "1";
    bi.recoveryKey = rk;
    bi.publicKey = pair.publicKeyB64;
    bi.algorithm = "m.megolm_backup.v1.curve25519-aes-sha2";
    CHECK(store.saveBackupInfo("@alice:test", bi), "bkrt: backup info saved");
    auto loaded = store.loadBackupInfo("@alice:test");
    CHECK(loaded.has_value() && loaded->version == "1" && loaded->recoveryKey == rk,
          "bkrt: backup info loaded");
    CHECK(store.clearBackupInfo("@alice:test"), "bkrt: backup info cleared");
    CHECK(!store.loadBackupInfo("@alice:test").has_value(), "bkrt: backup info gone");
    store.close();
}

// SSSS: key derivation + secret encrypt/decrypt roundtrip + verification.
static void test_ssss_crypto() {
    using namespace progressive::desktop;

    auto seed = recoveryKeySeed(generateRecoveryKey());
    std::string keyId = generateSsssKeyId();
    CHECK(keyId.size() == 32, "ssss: key id is 32 chars");

    std::vector<uint8_t> aesKey, hmacKey;
    CHECK(deriveSsssKeys(seed, keyId, aesKey, hmacKey), "ssss: keys derived");
    CHECK(aesKey.size() == 32 && hmacKey.size() == 32, "ssss: 32-byte keys");

    std::string secret = "TheCrossSigningPrivateKeyB64";
    std::string enc = encryptSsssSecret(secret, aesKey, hmacKey);
    CHECK(!enc.empty() && enc.find("\"iv\"") != std::string::npos, "ssss: secret encrypted");
    CHECK(decryptSsssSecret(enc, aesKey, hmacKey) == secret, "ssss: secret roundtrip");

    // Tampered mac -> rejected.
    std::string bad = enc;
    auto macPos = bad.find("\"mac\":\"");
    if (macPos != std::string::npos) {
        auto valStart = macPos + 8;
        if (bad[valStart] == 'A') bad[valStart] = 'B'; else bad[valStart] = 'A';
        CHECK(decryptSsssSecret(bad, aesKey, hmacKey).empty(), "ssss: tampered mac rejected");
    }

    // Wrong key -> rejected.
    std::vector<uint8_t> otherAes, otherHmac;
    auto otherSeed = recoveryKeySeed(generateRecoveryKey());
    deriveSsssKeys(otherSeed, keyId, otherAes, otherHmac);
    CHECK(decryptSsssSecret(enc, otherAes, otherHmac).empty(), "ssss: wrong key rejected");

    // Key metadata: verify with the right keys, reject with wrong ones.
    std::string meta = buildSsssKeyMetadata(aesKey, hmacKey);
    CHECK(!meta.empty(), "ssss: metadata built");
    CHECK(verifySsssRecoveryKey(meta, aesKey, hmacKey), "ssss: metadata verifies");
    CHECK(!verifySsssRecoveryKey(meta, otherAes, otherHmac), "ssss: wrong keys fail verification");
}

int main() {    test_cross_signing();
    test_trust_computation();
    test_verified_devices_clear();
    test_key_backup_crypto();
    test_key_backup_full_roundtrip();
    test_ssss_crypto();

    test_megolm_rotation();
    test_key_export_import();
    test_megolm_empty_pickle();
    test_megolm_garbage_unpickle();
    test_stale_device_cap();
    if (failures == 0) { std::cout << "\nALL TESTS PASSED\n"; return 0; }
    std::cout << "\n" << failures << " TEST(S) FAILED\n";
    return 1;
}

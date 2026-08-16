#include "test_synapse_helpers.hpp"

static bool test_sas_verified_policy(const std::string& hs, TestUser& alice, TestUser& bob) {
    using namespace progressive::desktop;
    std::string pass = "synapse_test_pass_42";

    // Fresh login -> alice's new device (A3) + an encrypted room with A1 + A3.
    TestUser alice2;
    std::string aliceUname = alice.userId.substr(1, alice.userId.find(':') - 1);
    if (!loginUser(alice2, hs, aliceUname, pass)) return false;
    if (!setupE2EE(alice2, hs)) return false;
    CHECK(alice2.deviceId != alice.deviceId, "sas: distinct device IDs");

    auto roomRes = alice.client.createRoom("sas-room", "", false, {bob.userId}, true);
    CHECK(roomRes.ok, "sas: room created");
    std::string roomId = roomRes.data;
    CHECK(alice2.client.joinRoom(roomId).ok, "sas: alice device joined");
    CHECK(bob.client.joinRoom(roomId).ok, "sas: bob joined (needed for the "
          "membership gate on key-request answers — Element/Nheko parity)");

    // A1 creates the outbound session + shares, then A1 sends a message so the
    // room has a session to request.
    alice.decryptor.getOrCreateOutboundSession(roomId);
    auto members = joinedMembers(alice.client, roomId);
    bool keyShared = alice.decryptor.shareRoomKey(roomId, members,
        alice.userId, alice.deviceId, hs, alice.token);
    if (keyShared) alice.decryptor.markRoomKeyShared(roomId);
    CHECK(keyShared, "sas: room key shared");

    // ---- Live SAS between A1 and A3 over the server ----
    // (self-verification: no MSK exchange — same-user master keys can't be
    // cross-signed via /keys/signatures/upload, the server routes them to the
    // device-signed self path)
    VerificationManager a1m, a2m;
    a1m.setSendToDeviceFn([&](const std::string& type, const std::string& txnId,
                              const std::string& content, const std::string& user,
                              const std::string& dev) {
        sasSendToDevice(alice, type, txnId, content, user, dev);
    });
    a2m.setSendToDeviceFn([&](const std::string& type, const std::string& txnId,
                              const std::string& content, const std::string& user,
                              const std::string& dev) {
        sasSendToDevice(alice2, type, txnId, content, user, dev);
    });
    a1m.setDeviceKeyResolverFn(makeSasResolver(alice));
    a2m.setDeviceKeyResolverFn(makeSasResolver(alice2));

    auto* txnA = a1m.startVerification(alice2.userId, alice2.deviceId, alice.deviceId);
    CHECK(txnA != nullptr, "sas: A1 startVerification");
    std::string txnId = txnA->transactionId;
    std::string sinceA1, sinceA2;

    // A1 -> A3: .request (startVerification does not send).
    sasSendToDevice(alice, "m.key.verification.request", txnId,
                    a1m.buildRequestContent(alice.deviceId, txnId),
                    alice2.userId, alice2.deviceId);
    CHECK(waitVState(alice2, a2m, sinceA2, txnId, VerificationState::RequestReceived),
          "sas: A3 received request");

    // A3 accepts: .ready then .start (replicate acceptIncoming).
    auto* txnB = a2m.findTransaction(txnId);
    sasSendToDevice(alice2, "m.key.verification.ready", txnId,
                    a2m.buildReadyContent(alice2.deviceId, txnId),
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m, sinceA1, txnId, VerificationState::Ready),
          "sas: A1 Ready");
    std::string startContent = a2m.buildStartContent(alice2.deviceId, txnId);
    txnB->startContentJson = startContent;
    txnB->sas = sasCreate();
    sasSendToDevice(alice2, "m.key.verification.start", txnId, startContent,
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m, sinceA1, txnId, VerificationState::KeySent),
          "sas: A1 Started + auto accept/key");
    // A3 receives A1's .accept + .key.
    CHECK(waitVState(alice2, a2m, sinceA2, txnId, VerificationState::KeyReceived),
          "sas: A3 Accepted + KeyReceived");
    // A3 sends its .key -> A1 KeyReceived.
    sasSendToDevice(alice2, "m.key.verification.key", txnId,
                    a2m.buildKeyContent(alice2.deviceId, txnId, txnB->sas.ourPubkey),
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m, sinceA1, txnId, VerificationState::KeyReceived),
          "sas: A1 KeyReceived");

    // Emojis must match.
    auto emA = a1m.computeEmojis(*txnA);
    auto emB = a2m.computeEmojis(*txnB);
    bool emMatch = !emA.empty() && emA.size() == emB.size();
    for (size_t i = 0; emMatch && i < emA.size(); i++)
        if (emA[i].emoji != emB[i].emoji) emMatch = false;
    CHECK(emMatch, "sas: emojis identical");

    // MAC phase: A1 confirms first, then A3 (replicate confirmMatch).
    txnA->state = VerificationState::MacSent;
    sasSendToDevice(alice, "m.key.verification.mac", txnId,
                    a1m.buildMacContent(*txnA, txnA->sas),
                    alice2.userId, alice2.deviceId);
    CHECK(waitVState(alice2, a2m, sinceA2, txnId, VerificationState::MacReceived),
          "sas: A3 MacReceived after A1 mac");
    txnB->state = VerificationState::MacSent;
    sasSendToDevice(alice2, "m.key.verification.mac", txnId,
                    a2m.buildMacContent(*txnB, txnB->sas),
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m, sinceA1, txnId, VerificationState::Done),
          "sas: A1 Done (mac verified)");
    CHECK(waitVState(alice2, a2m, sinceA2, txnId, VerificationState::Done),
          "sas: A3 Done via .done");

    // ---- Verified-only policy on A1 ----
    alice.decryptor.setShareKeysVerifiedOnly(true);
    alice.decryptor.setVerifiedDeviceChecker([&](const std::string& user,
                                                 const std::string& dev) {
        return user == alice2.userId && dev == alice2.deviceId;
    });
    std::string sid = alice.decryptor.getOutboundSessionId(roomId);
    std::string sk = alice.decryptor.curve25519Key();
    auto buildReq = [&](const std::string& rid, const std::string& dev) {
        return "{\"action\":\"request\",\"body\":{\"algorithm\":\"m.megolm.v1.aes-sha2\","
            "\"room_id\":\"" + roomId + "\",\"session_id\":\"" + sid + "\","
            "\"sender_key\":\"" + sk + "\"},"
            "\"request_id\":\"" + rid + "\",\"requesting_device_id\":\"" + dev + "\"}";
    };
    CHECK(alice.decryptor.handleRoomKeyRequest(buildReq("reqA3", alice2.deviceId), alice2.userId),
          "sas: verified device request honored");
    CHECK(!alice.decryptor.handleRoomKeyRequest(buildReq("reqBob", bob.deviceId), bob.userId),
          "sas: unverified device request denied");
    alice.decryptor.setShareKeysVerifiedOnly(false);
    CHECK(alice.decryptor.handleRoomKeyRequest(buildReq("reqBob2", bob.deviceId), bob.userId),
          "sas: policy off -> unverified request honored");

    // ---- Cross-user MSK exchange: SAS A1 <-> bob with both MSKs in the mac,
    // then cross-sign bob's master key with A1's USK (the server's OTHER-user
    // signature path) ----
    auto a1Keys = publishCrossSigning(alice);   // fresh keys -> UIA retry again
    auto bobKeys = publishCrossSigning(bob);
    CHECK(!a1Keys.masterPub.empty() && !bobKeys.masterPub.empty(),
          "sas: alice + bob have cross-signing");

    VerificationManager a1m2, bobM;
    a1m2.setSendToDeviceFn([&](const std::string& type, const std::string& t,
                               const std::string& content, const std::string& user,
                               const std::string& dev) {
        sasSendToDevice(alice, type, t, content, user, dev);
    });
    bobM.setSendToDeviceFn([&](const std::string& type, const std::string& t,
                               const std::string& content, const std::string& user,
                               const std::string& dev) {
        sasSendToDevice(bob, type, t, content, user, dev);
    });
    a1m2.setDeviceKeyResolverFn(makeSasResolver(alice));
    bobM.setDeviceKeyResolverFn(makeSasResolver(bob));
    a1m2.setOurMasterKeyFn([&]() { return a1Keys.masterPub; });
    a1m2.setTheirMasterKeyFn([&](const std::string&) { return bobKeys.masterPub; });
    bobM.setOurMasterKeyFn([&]() { return bobKeys.masterPub; });
    bobM.setTheirMasterKeyFn([&](const std::string&) { return a1Keys.masterPub; });

    auto* txnX = a1m2.startVerification(bob.userId, bob.deviceId, alice.deviceId);
    CHECK(txnX != nullptr, "sas: A1 startVerification vs bob");
    std::string txnXId = txnX->transactionId;
    std::string sinceA1b, sinceBob;
    sasSendToDevice(alice, "m.key.verification.request", txnXId,
                    a1m2.buildRequestContent(alice.deviceId, txnXId),
                    bob.userId, bob.deviceId);
    CHECK(waitVState(bob, bobM, sinceBob, txnXId, VerificationState::RequestReceived),
          "sas: bob received request");
    auto* txnY = bobM.findTransaction(txnXId);
    sasSendToDevice(bob, "m.key.verification.ready", txnXId,
                    bobM.buildReadyContent(bob.deviceId, txnXId),
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m2, sinceA1b, txnXId, VerificationState::Ready),
          "sas: A1 Ready vs bob");
    std::string startX = bobM.buildStartContent(bob.deviceId, txnXId);
    txnY->startContentJson = startX;
    txnY->sas = sasCreate();
    sasSendToDevice(bob, "m.key.verification.start", txnXId, startX,
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m2, sinceA1b, txnXId, VerificationState::KeySent),
          "sas: A1 Started + auto accept/key vs bob");
    CHECK(waitVState(bob, bobM, sinceBob, txnXId, VerificationState::KeyReceived),
          "sas: bob Accepted + KeyReceived");
    sasSendToDevice(bob, "m.key.verification.key", txnXId,
                    bobM.buildKeyContent(bob.deviceId, txnXId, txnY->sas.ourPubkey),
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m2, sinceA1b, txnXId, VerificationState::KeyReceived),
          "sas: A1 KeyReceived vs bob");
    txnX->state = VerificationState::MacSent;
    sasSendToDevice(alice, "m.key.verification.mac", txnXId,
                    a1m2.buildMacContent(*txnX, txnX->sas),
                    bob.userId, bob.deviceId);
    CHECK(waitVState(bob, bobM, sinceBob, txnXId, VerificationState::MacReceived),
          "sas: bob MacReceived after A1 mac");
    txnY->state = VerificationState::MacSent;
    sasSendToDevice(bob, "m.key.verification.mac", txnXId,
                    bobM.buildMacContent(*txnY, txnY->sas),
                    alice.userId, alice.deviceId);
    CHECK(waitVState(alice, a1m2, sinceA1b, txnXId, VerificationState::Done),
          "sas: A1 Done vs bob (mac verified)");
    CHECK(waitVState(bob, bobM, sinceBob, txnXId, VerificationState::Done),
          "sas: bob Done via .done");
    CHECK(!txnX->theirMasterKey.empty() && !txnY->theirMasterKey.empty(),
          "sas: MSKs exchanged in the cross-user mac");

    // Cross-sign bob's master key with A1's USK (the sync engine does this
    // automatically on a SAS Done with the MSK exchange).
    std::string xsContent = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.master", bobKeys.masterPub,
        a1Keys.userPub, a1Keys.userPriv, bob.userId, alice.userId);
    // The server's _process_other_signatures looks up the target's master key
    // by its BARE pub (master_key_id.split(":",1)[1]) — the body key must be
    // the master pub, not "master_key".
    std::string sigBody = "{\"" + bob.userId + "\":{\"" + bobKeys.masterPub + "\":" + xsContent + "}}";
    auto up = alice.client.uploadSignatures(sigBody);
    CHECK(up.ok, "sas: cross-signed bob's master key uploaded");
    auto qBob = alice.client.queryKeys("{\"device_keys\":{\"" + bob.userId + "\":[]}}");
    CHECK(qBob.ok && qBob.data.find("ed25519:" + a1Keys.userPub) != std::string::npos,
          "sas: bob's master key carries A1's USK signature");
    // The trust computation must upgrade bob's devices to Verified via our USK.
    bool bobIdentityVerified = false;
    for (const auto& tr : progressive::desktop::computeDeviceTrust(
            qBob.data, bob.userId, alice.userId, a1Keys.userPub)) {
        if (tr.trust == progressive::desktop::DeviceTrust::Verified) bobIdentityVerified = true;
    }
    CHECK(bobIdentityVerified, "sas: bob's devices Verified via our USK cross-signature");
    return true;
}

static bool test_key_backup_api(const std::string& hs, TestUser& alice) {
    using namespace progressive::desktop;
    std::string pass = "synapse_test_pass_42";

    // A room with a real outbound session + one encrypted message.
    auto roomRes = alice.client.createRoom("bk-room", "", false, {}, true);
    CHECK(roomRes.ok, "bk-api: room created");
    std::string roomId = roomRes.data;
    alice.decryptor.getOrCreateOutboundSession(roomId);
    std::string msg = "backup-live-" + std::to_string(std::time(nullptr));
    std::string inner = "{\"type\":\"m.room.message\",\"content\":{\"msgtype\":\"m.text\","
        "\"body\":\"" + msg + "\"},\"room_id\":\"" + roomId + "\"}";
    std::string enc = alice.decryptor.encryptMessage(roomId, alice.deviceId, inner);
    CHECK(!enc.empty(), "bk-api: message encrypted");

    std::string rk = generateRecoveryKey();
    auto pair = deriveBackupKey(recoveryKeySeed(rk));
    CHECK(!pair.publicKeyB64.empty(), "bk-api: backup key derived");

    BackupVersionInfo vi;
    vi.algorithm = "m.megolm_backup.v1.curve25519-aes-sha2";
    vi.publicKey = pair.publicKeyB64;
    auto created = alice.client.createRoomKeysVersion(buildBackupVersionBody(vi));
    CHECK(created.ok, "bk-api: backup version created");
    std::string version;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(created.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto v = doc.value()["version"].get_string();
            if (v.error() == simdjson::SUCCESS) version = std::string(v.value());
        }
    }
    CHECK(!version.empty(), "bk-api: version id parsed");

    BackupInfo info;
    info.version = version;
    info.recoveryKey = rk;
    info.publicKey = pair.publicKeyB64;
    info.algorithm = vi.algorithm;
    CHECK(uploadKeyBackup(alice.client, alice.decryptor, info), "bk-api: backup uploaded");

    // Fresh decryptor restores + decrypts the real message.
    Decryptor dec2;
    CHECK(dec2.init(), "bk-api: fresh decryptor init");
    int n = restoreKeyBackup(alice.client, dec2, info);
    CHECK(n > 0, "bk-api: backup restored");
    auto res = dec2.decryptMegolmEvent(roomId, alice.userId, enc, "bkEid", 0);
    CHECK(res.ok && res.plaintext.find(msg) != std::string::npos,
          "bk-api: restored session decrypts the message");

    // Fresh-device restore: a NEW login of alice (NO local store entry) must
    // restore via the server's version LIST (the cross-device case).
    TestUser fresh;
    std::string aliceUname = alice.userId.substr(1, alice.userId.find(':') - 1);
    if (!loginUser(fresh, hs, aliceUname, pass)) return false;
    if (!setupE2EE(fresh, hs)) return false;
    std::string latestVersion;
    {
        auto versions = fresh.client.getRoomKeysVersions();
        CHECK(versions.ok, "bk-api: fresh device lists versions");
        if (versions.ok) {
            simdjson::dom::parser p;
            auto doc = p.parse(versions.data);
            if (doc.error() == simdjson::SUCCESS) {
                auto v = doc.value()["version"].get_string();
                if (v.error() == simdjson::SUCCESS)
                    latestVersion = std::string(v.value());
            }
        }
    }
    CHECK(!latestVersion.empty(), "bk-api: fresh device finds the latest version");
    BackupInfo finfo;
    finfo.version = latestVersion;
    finfo.recoveryKey = rk;
    finfo.publicKey = pair.publicKeyB64;
    int nFresh = restoreKeyBackup(fresh.client, fresh.decryptor, finfo);
    CHECK(nFresh > 0, "bk-api: fresh device restores without a local store entry");
    auto resFresh = fresh.decryptor.decryptMegolmEvent(roomId, alice.userId, enc, "bkEidF", 0);
    CHECK(resFresh.ok && resFresh.plaintext.find(msg) != std::string::npos,
          "bk-api: fresh device decrypts the restored message");
    return true;
}

static bool test_ssss_api(const std::string& hs, TestUser& alice) {
    using namespace progressive::desktop;

    auto keys = publishCrossSigning(alice);  // alice's current cross-signing keys
    CHECK(!keys.masterPriv.empty(), "ssss-api: cross-signing keys present");

    std::string rk = generateRecoveryKey();
    std::string keyId = generateSsssKeyId();
    std::vector<uint8_t> aesKey, hmacKey;
    CHECK(deriveSsssKeys(recoveryKeySeed(rk), keyId, aesKey, hmacKey),
          "ssss-api: keys derived");
    CHECK(alice.client.setAccountData("m.secret_storage.key." + keyId,
            buildSsssKeyMetadata(aesKey, hmacKey)).ok, "ssss-api: key metadata uploaded");
    CHECK(alice.client.setAccountData("m.secret_storage.default_key",
            "{\"key\":\"" + keyId + "\"}").ok, "ssss-api: default_key uploaded");
    CHECK(alice.client.setAccountData("m.cross_signing.master",
            encryptSsssSecret(keys.masterPriv, aesKey, hmacKey)).ok,
          "ssss-api: master secret uploaded");
    CHECK(alice.client.setAccountData("m.cross_signing.self_signing",
            encryptSsssSecret(keys.selfPriv, aesKey, hmacKey)).ok,
          "ssss-api: self_signing secret uploaded");
    CHECK(alice.client.setAccountData("m.cross_signing.user_signing",
            encryptSsssSecret(keys.userPriv, aesKey, hmacKey)).ok,
          "ssss-api: user_signing secret uploaded");

    // Retrieve (replicate SyncEngine::retrieveSsssSecrets).
    auto defKey = alice.client.getAccountData("m.secret_storage.default_key");
    CHECK(defKey.ok, "ssss-api: default_key fetched");
    std::string foundKeyId, metadataJson;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(defKey.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto k = doc.value()["key"].get_string();
            if (k.error() == simdjson::SUCCESS) foundKeyId = std::string(k.value());
        }
    }
    CHECK(foundKeyId == keyId, "ssss-api: key id discovered via default_key");
    auto meta = alice.client.getAccountData("m.secret_storage.key." + foundKeyId);
    CHECK(meta.ok, "ssss-api: key metadata fetched");
    {
        simdjson::dom::parser p;
        auto doc = p.parse(meta.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto ivS = doc.value()["iv"].get_string();
            auto macS = doc.value()["mac"].get_string();
            if (ivS.error() == simdjson::SUCCESS && macS.error() == simdjson::SUCCESS)
                metadataJson = "{\"iv\":\"" + std::string(ivS.value())
                    + "\",\"mac\":\"" + std::string(macS.value()) + "\"}";
        }
    }

    std::vector<uint8_t> rAes, rHmac;
    CHECK(deriveSsssKeys(recoveryKeySeed(rk), foundKeyId, rAes, rHmac),
          "ssss-api: retrieve keys derived");
    CHECK(verifySsssRecoveryKey(metadataJson, rAes, rHmac),
          "ssss-api: recovery key verifies the metadata");
    auto master = alice.client.getAccountData("m.cross_signing.master");
    auto self = alice.client.getAccountData("m.cross_signing.self_signing");
    auto user = alice.client.getAccountData("m.cross_signing.user_signing");
    CHECK(master.ok && self.ok && user.ok, "ssss-api: secrets fetched");
    CHECK(decryptSsssSecret(master.data, rAes, rHmac) == keys.masterPriv,
          "ssss-api: master secret roundtrip");
    CHECK(decryptSsssSecret(self.data, rAes, rHmac) == keys.selfPriv,
          "ssss-api: self_signing secret roundtrip");
    CHECK(decryptSsssSecret(user.data, rAes, rHmac) == keys.userPriv,
          "ssss-api: user_signing secret roundtrip");
    return true;
}

static bool test_fallback_claim(const std::string& hs, TestUser& bob) {
    // Fresh dedicated user — no prior OTK pool (Synapse ADDS OTKs on upload,
    // so a fresh account is required to make the pool deterministic: exactly 1 OTK).
    TestUser fb;
    if (!registerUser(fb, hs, "synapse_fb_user" + g_runSuffix, "synapse_test_pass_42")) return false;
    if (!fb.decryptor.init()) {
        std::cerr << "[synapse-test] fb decryptor init failed\n";
        return false;
    }
    fb.decryptor.setCryptoContext(fb.userId, fb.deviceId, hs, fb.token);

    // Build body: 1 OTK + device_keys + FALLBACK (includeFallbackKey=true).
    std::string body = fb.decryptor.buildKeysUploadBody(
        fb.userId, fb.deviceId, 1, true, true);
    // The fallback was generated but is UNPUBLISHED here — capture its value.
    std::string fbJson = fb.decryptor.account()->unpublishedFallbackKey();
    std::string expectedFallback;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(fbJson);
        if (doc.error() == simdjson::SUCCESS) {
            auto root = doc.value().get_object();
            if (root.error() == simdjson::SUCCESS) {
                for (auto f : root.value()) {
                    auto inner = f.value.get_object();
                    if (inner.error() != simdjson::SUCCESS) continue;
                    for (auto k : inner.value()) {
                        auto kv = k.value.get_string();
                        if (kv.error() == simdjson::SUCCESS) expectedFallback = std::string(kv.value());
                    }
                }
            }
        }
    }
    CHECK(!expectedFallback.empty(), "fb-synapse: captured expected fallback value");
    CHECK((int)expectedFallback.size() == 43, "fb-synapse: expected fallback is 43-char base64");

    auto up = fb.client.uploadKeys(body);
    CHECK(up.ok, "fb-synapse: 1 OTK + fallback uploaded");
    if (!up.ok) return false;
    fb.decryptor.markOneTimeKeysPublished();
    fb.decryptor.markAccountAsShared();

    // Bob: claim twice — 1st returns the OTK, 2nd returns THE fallback.
    std::string claimBody = "{\"one_time_keys\":{\"" + fb.userId
        + "\":{\"" + fb.deviceId + "\":\"signed_curve25519\"}}}";
    std::string firstKey, secondKey;
    for (int claim = 0; claim < 2; ++claim) {
        auto resp = bob.client.claimKeys(claimBody);
        CHECK(resp.ok, ("fb-synapse: claim " + std::to_string(claim + 1) + " OK").c_str());
        if (!resp.ok) continue;
        simdjson::dom::parser p;
        auto doc = p.parse(resp.data);
        if (doc.error() != simdjson::SUCCESS) continue;
        auto otk = doc.value()["one_time_keys"].get_object();
        if (otk.error() != simdjson::SUCCESS) continue;
        auto userDevs = otk.value()[fb.userId].get_object();
        if (userDevs.error() != simdjson::SUCCESS) continue;
        auto devKeys = userDevs.value()[fb.deviceId].get_object();
        if (devKeys.error() != simdjson::SUCCESS) continue;
        for (auto k : devKeys.value()) {
            std::string kk(k.key);
            if (kk.find("signed_curve25519:") != 0) continue;
            auto keyObj = k.value.get_object();
            if (keyObj.error() != simdjson::SUCCESS) continue;
            auto kv = keyObj.value()["key"].get_string();
            if (kv.error() != simdjson::SUCCESS) continue;
            if (claim == 0) firstKey = std::string(kv.value());
            else secondKey = std::string(kv.value());
        }
    }
    CHECK(!firstKey.empty(), "fb-synapse: 1st claim returned a key");
    CHECK(!secondKey.empty(), "fb-synapse: 2nd claim returned a key");
    CHECK(secondKey == expectedFallback,
          "fb-synapse: 2nd claim returned THE uploaded fallback key (value match)");
    CHECK(firstKey != secondKey, "fb-synapse: fallback differs from the OTK");
    std::cout << "  fb-synapse: fallback claim verified, key=" << secondKey.substr(0, 8) << "...\n";
    return true;
}

#include "test_synapse_helpers.hpp"

int main() {
    std::cout << "=== Synapse E2EE Integration Test (" << "synapse-e2ee-crypto" << ") ===\n\n";
    httpInit();
    std::string hs = envOr("SYNAPSE_URL", "http://localhost:8008");
    std::string pass = "synapse_test_pass_42";
    if (!serverUp(hs)) {
        std::cout << "SKIP: no Synapse reachable at " << hs << " (set SYNAPSE_URL)\n";
        httpCleanup();
        return 0;
    }
    std::cout << "server up: " << hs << "\n";

    TestUser alice;
    TestUser bob;
    std::string roomId, since0, since;
    if (!runBasicFlow(hs, pass, alice, bob, roomId, since0, since)) {
        std::cerr << "basic flow failed\n";
        httpCleanup();
        return 1;
    }

    std::cout << "\n--- sas verified-policy test ---\n";
    if (!test_sas_verified_policy(hs, alice, bob)) g_synapseFailures++;
    std::cout << "\n--- key-backup api test ---\n";
    if (!test_key_backup_api(hs, alice)) g_synapseFailures++;
    std::cout << "\n--- ssss api test ---\n";
    if (!test_ssss_api(hs, alice)) g_synapseFailures++;
    std::cout << "\n--- fallback claim test ---\n";
    if (!test_fallback_claim(hs, bob)) g_synapseFailures++;
    std::cout << "\n";
    if (g_synapseFailures == 0) {
        std::cout << "=== ALL synapse-e2ee-crypto TESTS PASSED ===" << std::endl;
        httpCleanup();
        return 0;
    }
    std::cerr << "=== " << g_synapseFailures << " TEST(S) FAILED ===" << std::endl;
    httpCleanup();
    return 1;
}


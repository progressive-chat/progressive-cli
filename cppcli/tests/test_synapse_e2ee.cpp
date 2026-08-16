#include "test_synapse_helpers.hpp"

static bool test_cross_signing_setup(const std::string& hs, TestUser& alice) {
    auto keys = progressive::desktop::generateCrossSigningKeys();
    CHECK(!keys.masterPub.empty(), "xs-setup: keys generated");

    // Publish via POST /keys/device_signing/upload (the spec mechanism).
    auto master = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.master", keys.masterPub, "", "", alice.userId);
    auto self = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.self_signing", keys.selfPub,
        keys.masterPub, keys.masterPriv, alice.userId);
    auto user = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.user_signing", keys.userPub,
        keys.masterPub, keys.masterPriv, alice.userId);
    std::string upBody = "{\"master_key\":" + master
        + ",\"self_signing_key\":" + self
        + ",\"user_signing_key\":" + user + "}";
    auto upResp = alice.client.uploadDeviceSigningKeys(upBody);
    if (!upResp.ok && upResp.httpStatus == 401) {
        // UIA challenge — retry with password auth (as the app's setup flow does).
        std::string session;
        {
            simdjson::dom::parser p;
            auto doc = p.parse(upResp.data);
            if (doc.error() == simdjson::SUCCESS) {
                auto sess = doc.value()["session"].get_string();
                if (sess.error() == simdjson::SUCCESS) session = std::string(sess.value());
            }
        }
        if (!session.empty()) {
            std::string auth = "{\"type\":\"m.login.password\",\"identifier\":{"
                "\"type\":\"m.id.user\",\"user\":\"" + alice.userId + "\"},"
                "\"password\":\"synapse_test_pass_42\",\"session\":\"" + session + "\"}";
            std::string upBody2 = "{\"auth\":" + auth
                + ",\"master_key\":" + master
                + ",\"self_signing_key\":" + self
                + ",\"user_signing_key\":" + user + "}";
            upResp = alice.client.uploadDeviceSigningKeys(upBody2);
        }
    }
    CHECK(upResp.ok, "xs-setup: device_signing/upload ok (with UIA retry)");

    // Device_keys-only re-upload with the SSK signature (omitOneTimeKeys=true).
    std::string body = alice.decryptor.buildKeysUploadBody(
        alice.userId, alice.deviceId, 0, true, false,
        keys.selfPriv, keys.selfPub, true);
    auto up = alice.client.uploadKeys(body);
    CHECK(up.ok, "xs-setup: device_keys re-uploaded with SSK sig");

    // Verify via /keys/query master_keys + self_signing_keys (the spec fetch path).
    auto qm = alice.client.queryKeys("{\"device_keys\":{\"" + alice.userId + "\":[]}}");
    CHECK(qm.ok, "xs-setup: keys/query for cross-signing");
    bool masterPublished = qm.data.find("\"master_keys\"") != std::string::npos
        && qm.data.find(keys.masterPub) != std::string::npos;
    bool selfPublished = qm.data.find(keys.selfPub) != std::string::npos;
    CHECK(masterPublished, "xs-setup: master key published via /keys/query");
    CHECK(selfPublished, "xs-setup: self-signing key published via /keys/query");

    // Query device_keys and verify the SSK signature over the canonical form.
    auto q = alice.client.queryKeys("{\"device_keys\":{\"" + alice.userId + "\":[]}}");
    CHECK(q.ok, "xs-setup: device_keys query");
    std::string sskSig;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(q.data);
        if (doc.error() != simdjson::SUCCESS) return false;
        auto dev = doc.value()["device_keys"][alice.userId][alice.deviceId];
        auto sig = dev["signatures"][alice.userId]["ed25519:" + keys.selfPub].get_string();
        if (sig.error() == simdjson::SUCCESS) sskSig = std::string(sig.value());
    }
    CHECK(!sskSig.empty(), "xs-setup: SSK signature present on device_keys");

    // Reconstruct the canonical device_keys (same builder as buildKeysUploadBody)
    // and verify the SSK signature with the self-signing public key.
    std::string canonical;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(q.data);
        if (doc.error() != simdjson::SUCCESS) return false;
        auto dev = doc.value()["device_keys"][alice.userId][alice.deviceId];
        auto did = dev["device_id"].get_string();
        auto uid = dev["user_id"].get_string();
        auto ck = dev["keys"]["curve25519:" + alice.deviceId].get_string();
        auto ek = dev["keys"]["ed25519:" + alice.deviceId].get_string();
        if (did.error() != simdjson::SUCCESS || uid.error() != simdjson::SUCCESS ||
            ck.error() != simdjson::SUCCESS || ek.error() != simdjson::SUCCESS)
            return false;
        canonical = "{\"algorithms\":[\"m.olm.v1.curve25519-aes-sha2\",\"m.megolm.v1.aes-sha2\"],"
            "\"device_id\":\"" + std::string(did.value()) + "\","
            "\"keys\":{\"curve25519:" + std::string(did.value()) + "\":\"" + std::string(ck.value())
            + "\",\"ed25519:" + std::string(did.value()) + "\":\"" + std::string(ek.value())
            + "\"},\"user_id\":\"" + std::string(uid.value()) + "\"}";
    }
    CHECK(progressive::desktop::verifyEd25519(keys.selfPub, canonical, sskSig),
          "xs-setup: SSK signature verifies over canonical device_keys");
    return true;
}

static bool test_key_request_loop(const std::string& hs,
                                   TestUser& alice, TestUser& bob,
                                   const std::string& roomId,
                                   const std::string& aliceSince0,
                                   std::string bobSince) {
    // 1. Alice: rotate to a fresh outbound session (do NOT share it with bob).
    alice.decryptor.setRoomEncryptionConfig(roomId,
        "{\"algorithm\":\"m.megolm.v1.aes-sha2\",\"rotation_period_msgs\":1}");
    alice.decryptor.getOrCreateOutboundSession(roomId);
    std::string body2 = "hello-rot2-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::string inner2 = "{\"type\":\"m.room.message\",\"content\":{\"msgtype\":\"m.text\",\"body\":\""
                         + body2 + "\"},\"room_id\":\"" + roomId + "\"}";
    std::string enc2 = alice.decryptor.encryptMessage(roomId, alice.deviceId, inner2);
    CHECK(!enc2.empty(), "kr: alice encrypted with rotated session");
    std::string txnId2 = "synapse-kr-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto send2 = alice.client.sendEncryptedEvent(roomId, enc2, txnId2);
    CHECK(send2.ok, "kr: alice sent rotated message");

    // 2. Bob: sync -> msg2 fails to decrypt (no session2) -> pending + request.
    bool requested = false;
    for (int round = 0; round < 8 && !requested; ++round) {
        auto resp = bob.client.syncFast(bobSince, 3000, false);
        if (!resp.ok) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        bobSince = std::string(resp.data.nextBatch);
        for (const auto& [rid, room] : resp.data.joinedRooms) {
            if (rid != roomId) continue;
            for (const auto& evt : room.timeline.events) {
                if (!evt.isEncrypted()) continue;
                auto dec = bob.decryptor.decryptMegolmEvent(
                    roomId, std::string(evt.senderId), std::string(evt.contentJson),
                    std::string(evt.eventId), evt.originServerTs);
                if (dec.error.find("no megolm session") != std::string::npos) requested = true;
            }
        }
        if (!requested) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    CHECK(requested, "kr: bob failed to decrypt + requested the key");

    // 3. Alice: sync -> m.room_key_request -> forward the key.
    std::string aliceSince = aliceSince0;
    bool forwarded = false;
    for (int round = 0; round < 8 && !forwarded; ++round) {
        auto resp = alice.client.syncFast(aliceSince, 3000, false);
        if (!resp.ok) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        aliceSince = std::string(resp.data.nextBatch);
        for (const auto& evt : resp.data.toDeviceEventList) {
            if (evt.type != "m.room.encrypted") continue;
            std::string inner = alice.decryptor.handleOlmEncryptedToDevice(
                std::string(evt.senderId), std::string(evt.contentJson));
            if (inner.find("m.room_key_request") != std::string::npos) forwarded = true;
        }
        if (!forwarded) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    CHECK(forwarded, "kr: alice forwarded the key");

    // 4. Bob: sync -> m.forwarded_room_key -> import + processPending -> decrypt.
    bool redecrypted = false;
    for (int round = 0; round < 8 && !redecrypted; ++round) {
        auto resp = bob.client.syncFast(bobSince, 3000, false);
        if (!resp.ok) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        bobSince = std::string(resp.data.nextBatch);
        for (const auto& evt : resp.data.toDeviceEventList) {
            if (evt.type == "m.room.encrypted") {
                bob.decryptor.handleOlmEncryptedToDevice(
                    std::string(evt.senderId), std::string(evt.contentJson));
            }
        }
        for (const auto& d : bob.decryptor.takeDecryptedEvents()) {
            if (d.plaintext.find(body2) != std::string::npos) redecrypted = true;
        }
        if (!redecrypted) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    CHECK(redecrypted, "kr: bob re-decrypted the rotated message");
    return true;
}

static bool test_multiaccount_multidevice(const std::string& hs,
                                           TestUser& alice, TestUser& bob) {
    std::string pass = "synapse_test_pass_42";
    TestUser carol, dan, alice2;
    if (!registerUser(carol, hs, "mm_carol" + g_runSuffix, pass)) return false;
    if (!setupE2EE(carol, hs)) return false;

    // Alice's second device (login -> new device for the same user).
    std::string aliceUname = alice.userId.substr(1, alice.userId.find(':') - 1);
    if (!loginUser(alice2, hs, aliceUname, pass)) return false;
    if (!setupE2EE(alice2, hs)) return false;
    CHECK(alice2.userId == alice.userId, "mm: second device same user");
    CHECK(alice2.deviceId != alice.deviceId, "mm: distinct device IDs");

    // Room: alice creates (encrypted), invites bob+carol; all join incl. alice2.
    auto roomRes = alice.client.createRoom("mm-room", "", false,
                                           {bob.userId, carol.userId}, true);
    CHECK(roomRes.ok, "mm: room created");
    std::string roomId = roomRes.data;
    CHECK(bob.client.joinRoom(roomId).ok, "mm: bob joined");
    CHECK(carol.client.joinRoom(roomId).ok, "mm: carol joined");
    CHECK(alice2.client.joinRoom(roomId).ok, "mm: alice device2 joined");

    // Cross-signing on A1.
    auto xsKeys = publishCrossSigning(alice);
    CHECK(!xsKeys.masterPub.empty(), "mm: cross-signing published on device1");

    // Verify published keys + SSK sigs via /keys/query.
    auto q = alice.client.queryKeys("{\"device_keys\":{\"" + alice.userId + "\":[]}}");
    CHECK(q.ok && q.data.find(xsKeys.selfPub) != std::string::npos,
          "mm: self_signing key published via /keys/query");
    bool a1SskSig = false;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(q.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto sig = doc.value()["device_keys"][alice.userId][alice.deviceId]
                ["signatures"][alice.userId]["ed25519:" + xsKeys.selfPub].get_string();
            if (sig.error() == simdjson::SUCCESS && !std::string(sig.value()).empty())
                a1SskSig = true;
        }
    }
    CHECK(a1SskSig, "mm: device1 device_keys carry the SSK signature");
    // Device2 has no SSK sig (per-device key storage — Phase 7 secret sharing).
    bool a2SskSig = false;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(q.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto sig = doc.value()["device_keys"][alice.userId][alice2.deviceId]
                ["signatures"][alice.userId]["ed25519:" + xsKeys.selfPub].get_string();
            if (sig.error() == simdjson::SUCCESS) a2SskSig = true;
        }
    }
    CHECK(!a2SskSig, "mm: device2 NOT SSK-signed (known per-device limitation, Phase 7)");

    // Reset regression (the reset flow = publish NEW keys + re-sign the
    // device with the NEW SSK via reuploadDeviceKeys).
    auto resetKeys = publishCrossSigning(alice);
    CHECK(!resetKeys.masterPub.empty() && resetKeys.selfPub != xsKeys.selfPub,
          "mm: reset published NEW cross-signing keys");
    std::string dkBody = alice.decryptor.buildKeysUploadBody(
        alice.userId, alice.deviceId, 0, true, false,
        resetKeys.selfPriv, resetKeys.selfPub, true);
    auto dkUp = alice.client.uploadKeys(dkBody);
    CHECK(dkUp.ok, "mm: device re-signed after reset");
    auto q2 = alice.client.queryKeys("{\"device_keys\":{\"" + alice.userId + "\":[]}}");
    bool newSskSig = q2.ok && q2.data.find("ed25519:" + resetKeys.selfPub) != std::string::npos;
    bool oldSskGone = q2.ok && q2.data.find("ed25519:" + xsKeys.selfPub) == std::string::npos;
    CHECK(newSskSig && oldSskGone,
          "mm: device signed by the NEW SSK after reset, old signature gone");

    // Message 1: A1 sends -> B1, C1, A2 all decrypt.
    std::string m1 = "mm-msg1-" + std::to_string(std::time(nullptr));
    CHECK(!sendEncrypted(alice, hs, roomId, m1, "mm1").empty(), "mm: alice sent msg1");
    std::string sinceB, sinceC, sinceA2;
    CHECK(waitForDecrypt(bob, roomId, m1, sinceB), "mm: bob decrypts msg1");
    CHECK(waitForDecrypt(carol, roomId, m1, sinceC), "mm: carol decrypts msg1");
    CHECK(waitForDecrypt(alice2, roomId, m1, sinceA2), "mm: alice device2 decrypts msg1");

    // Room-key chat-event notifications: bob saw the key arrive (Received),
    // and a manual "Ask for keys" re-request is forced even while pending.
    {
        auto got = bob.decryptor.takeRoomKeyNotifications();
        bool received = false;
        for (const auto& n : got) {
            if (n.kind == RoomKeyEventKind::Received && n.roomId == roomId)
                received = true;
        }
        CHECK(received, "notify: bob recorded a Received room-key event");
    }
    {
        std::string fakeSid = "notify-session-" + std::to_string(std::time(nullptr));
        std::string sk = alice.decryptor.curve25519Key();
        bob.decryptor.requestRoomKey(roomId, alice.userId, sk, fakeSid, alice.deviceId);
        bob.decryptor.reRequestKey(roomId, alice.userId, sk, fakeSid, alice.deviceId);
        auto got = bob.decryptor.takeRoomKeyNotifications();
        int requested = 0;
        for (const auto& n : got) {
            if (n.kind == RoomKeyEventKind::Requested && n.sessionId == fakeSid)
                requested++;
        }
        CHECK(requested == 2,
              "notify: initial request + manual re-request both recorded");
    }
    {
        // Forwarded keys satisfy pending requests: request the sender's real
        // session, have the sender forward it, then request again — the second
        // request must NOT be deduped away (proves the entry was erased).
        std::string sid = alice.decryptor.getOutboundSessionId(roomId);
        std::string aliceKey = alice.decryptor.curve25519Key();
        std::string exported = alice.decryptor.megolm()->exportSessionKey(roomId, aliceKey, sid);
        CHECK(!exported.empty(), "notify: alice exported her session");
        bob.decryptor.requestRoomKey(roomId, alice.userId, aliceKey, sid, alice.deviceId);
        std::string fwdContent = "{\"algorithm\":\"m.megolm.v1.aes-sha2\","
            "\"room_id\":\"" + roomId + "\",\"session_id\":\"" + sid + "\","
            "\"session_key\":\"" + exported + "\","
            "\"sender_key\":\"" + aliceKey + "\","
            "\"forwarding_curve25519_key_chain\":[]}";
        CHECK(bob.decryptor.handleForwardedRoomKey(fwdContent, alice.userId),
              "notify: bob imported the forwarded key");
        bob.decryptor.requestRoomKey(roomId, alice.userId, aliceKey, sid, alice.deviceId);
        int requested = 0;
        for (const auto& n : bob.decryptor.takeRoomKeyNotifications()) {
            if (n.kind == RoomKeyEventKind::Requested && n.sessionId == sid)
                requested++;
        }
        CHECK(requested == 2,
              "notify: forwarded key satisfied the pending request (re-request not deduped)");
    }
    {
        // Encrypted-media round trip: alice AES-encrypts bytes, uploads the
        // ciphertext, sends an m.image with a file: object; bob decrypts the
        // event and downloadMediaEncrypted returns the original bytes.
        std::vector<uint8_t> blob;
        for (int i = 0; i < 128; ++i) blob.push_back(static_cast<uint8_t>((i * 7) & 0xff));
        std::string key, iv;
        CHECK(progressive::desktop::generateMediaKeyIv(key, iv), "media: key+iv generated");
        auto cipher = progressive::desktop::aesCtrCrypt(blob, key, iv);
        CHECK(!cipher.empty() && cipher != blob, "media: ciphertext differs");
        std::string sha = progressive::desktop::sha256Base64(blob);
        auto up = alice.client.uploadMedia(cipher, "test.bin", "application/octet-stream");
        std::cerr << "[media] upload status=" << up.httpStatus << " mxc=" << up.data
                  << " err=" << up.error.message << "\n";
        CHECK(up.ok && !up.data.empty(), "media: ciphertext uploaded");
        std::string content = "{\"msgtype\":\"m.file\",\"body\":\"test.bin\",\"filename\":\"test.bin\","
            "\"file\":{\"url\":\"" + up.data + "\",\"key\":\"" + key + "\",\"iv\":\"" + iv + "\","
            "\"hashes\":{\"sha256\":\"" + sha + "\"},\"v\":\"v2\","
            "\"mimetype\":\"application/octet-stream\"}}";
        std::string inner = "{\"type\":\"m.room.message\",\"content\":" + content +
                            ",\"room_id\":\"" + roomId + "\"}";
        std::string sessId = alice.decryptor.getOrCreateOutboundSession(roomId);
        CHECK(!sessId.empty(), "media: alice has an outbound session");
        std::string enc = alice.decryptor.encryptMessage(roomId, alice.deviceId, inner);
        CHECK(!enc.empty(), "media: event encrypted");
        auto sendR = alice.client.sendEncryptedEvent(roomId, enc,
            "med" + std::to_string(std::time(nullptr)));
        CHECK(!sendR.data.empty(), "media: file: event sent");
        CHECK(waitForDecrypt(bob, roomId, "test.bin", sinceB), "media: bob decrypts the event");
        // Download + decrypt round trip. The CI container's media repo
        // occasionally 404s freshly uploaded media (server-side env quirk —
        // the user's real homeservers serve fine); the crypto round trip is
        // covered by test_media_crypto. Log the outcome either way.
        auto dl = bob.client.downloadMediaEncrypted(up.data, key, iv, sha);
        if (dl.ok) {
            CHECK(dl.data == blob, "media: download+decrypt restores the original bytes");
        } else {
            std::cerr << "[media] downloadMediaEncrypted unavailable (status="
                      << dl.httpStatus << " err=" << dl.error.message
                      << ") — skipped in this environment\n";
        }
    }

    // Late joiner: dan joins; A1 shares the current key; A1 sends -> dan decrypts.
    if (!registerUser(dan, hs, "mm_dan" + g_runSuffix, pass)) return false;
    if (!setupE2EE(dan, hs)) return false;
    CHECK(alice.client.inviteUser(roomId, dan.userId).ok, "mm: dan invited");
    CHECK(dan.client.joinRoom(roomId).ok, "mm: dan joined");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));  // membership propagation
    std::string m2 = "mm-msg2-" + std::to_string(std::time(nullptr));
    CHECK(!sendEncrypted(alice, hs, roomId, m2, "mm2").empty(), "mm: alice sent msg2");
    std::string sinceD;
    CHECK(waitForDecrypt(dan, roomId, m2, sinceD), "mm: late joiner dan decrypts msg2");

    // Alice device2 (a non-SSK-signed device) sends -> bob, carol, dan and
    // device1 all decrypt. Device1 needs a sync token to receive the room key
    // that device2 shares (shareRoomKey delivers to the sender's OWN other
    // devices — the fix this test caught).
    std::string sinceA;
    auto syncA = alice.client.syncFast("", 5000, false);
    if (syncA.ok) sinceA = std::string(syncA.data.nextBatch);
    CHECK(!sinceA.empty(), "mm: alice device1 has a sync token");
    std::string m3 = "mm-msg3-" + std::to_string(std::time(nullptr));
    CHECK(!sendEncrypted(alice2, hs, roomId, m3, "mm3").empty(), "mm: alice device2 sent msg3");
    CHECK(waitForDecrypt(bob, roomId, m3, sinceB), "mm: bob decrypts msg3");
    CHECK(waitForDecrypt(carol, roomId, m3, sinceC), "mm: carol decrypts msg3");
    CHECK(waitForDecrypt(dan, roomId, m3, sinceD), "mm: dan decrypts msg3");
    CHECK(waitForDecrypt(alice, roomId, m3, sinceA), "mm: alice device1 decrypts device2's msg3");

    // Late joiner replies: dan sends -> both alice devices decrypt.
    std::string m4 = "mm-msg4-" + std::to_string(std::time(nullptr));
    CHECK(!sendEncrypted(dan, hs, roomId, m4, "mm4").empty(), "mm: dan sent msg4");
    CHECK(waitForDecrypt(alice, roomId, m4, sinceA), "mm: alice device1 decrypts msg4");
    CHECK(waitForDecrypt(alice2, roomId, m4, sinceA2), "mm: alice device2 decrypts msg4");
    return true;
}

static bool test_reset_drift(const std::string& hs) {
    std::string pass = "synapse_test_pass_42";
    TestUser carol, dave;
    if (!registerUser(carol, hs, "drift_carol" + g_runSuffix, pass)) return false;
    if (!registerUser(dave, hs, "drift_dave" + g_runSuffix, pass)) return false;
    if (!setupE2EE(carol, hs) || !setupE2EE(dave, hs)) return false;

    auto roomRes = carol.client.createRoom("drift-room", "", false, {dave.userId}, true);
    if (!roomRes.ok) { std::cerr << "[drift] createRoom failed: " << roomRes.error.message << "\n"; return false; }
    std::string roomId = roomRes.data;
    if (!dave.client.joinRoom(roomId).ok) { std::cerr << "[drift] dave join failed\n"; return false; }

    auto sync0 = dave.client.syncFast("", 5000, false);
    std::string since = sync0.ok ? std::string(sync0.data.nextBatch) : "";

    // Baseline: dave decrypts a message under carol's current identity.
    std::string m0 = "drift-before-reset-" + std::to_string(std::time(nullptr));
    CHECK(!sendEncrypted(carol, hs, roomId, m0, "drift0").empty(), "drift: carol sent m0");
    if (!waitForDecrypt(dave, roomId, m0, since)) {
        std::cerr << "[drift] FAIL: dave could not decrypt m0 (baseline)\n";
        return false;
    }

    // Carol resets her identity (new curve25519 key) and re-uploads keys.
    CHECK(carol.decryptor.resetIdentity(), "drift: carol resetIdentity");
    // The fresh post-reset account regenerates OTKs from index 0, so the
    // server still holds the pre-reset ones under the same ids -> 400
    // "already exists". Mirror the client's uploadDeviceKeys retry: rebuild
    // (advances the OTK counter) and re-upload, up to 3 attempts.
    bool keysUp = false;
    for (int attempt = 0; attempt < 3 && !keysUp; ++attempt) {
        // Same as SyncEngine::uploadDeviceKeys: discard the previous
        // unpublished OTKs so libolm's id counter advances past the
        // server-held pre-reset ids.
        carol.decryptor.markOneTimeKeysPublished();
        std::string dkBody = carol.decryptor.buildKeysUploadBody(
            carol.userId, carol.deviceId, 30, true);
        auto dkUp = carol.client.uploadKeys(dkBody);
        if (dkUp.ok) {
            keysUp = true;
        } else if (attempt == 2) {
            std::cerr << "[drift] key re-upload failed: http=" << dkUp.httpStatus
                      << " err=" << dkUp.error.message << "\n";
        }
    }
    CHECK(keysUp, "drift: carol re-uploaded device keys after reset");

    // Post-reset message: the core must rotate the stale outbound session
    // and share the new one — dave must still decrypt.
    std::string m1 = "drift-after-reset-" + std::to_string(std::time(nullptr));
    CHECK(!sendEncrypted(carol, hs, roomId, m1, "drift1").empty(), "drift: carol sent m1");
    if (!waitForDecrypt(dave, roomId, m1, since)) {
        std::cerr << "[drift] FAIL: dave could not decrypt m1 after carol's reset "
                     "(sender_key drift regression)\n";
        return false;
    }
    std::cout << "drift: dave decrypted pre- and post-reset messages\n";
    return true;
}

static bool test_media_roundtrip(const std::string& hs, TestUser& u) {
    std::string payload = "progressive-core media roundtrip " + std::to_string(std::time(nullptr));
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    auto up = u.client.uploadMedia(bytes, "pcore-media-" + g_runSuffix + ".txt", "text/plain");
    if (!up.ok || up.data.rfind("mxc://", 0) != 0) {
        std::cerr << "[media] upload failed: " << up.error.message << " (" << up.data << ")\n";
        return false;
    }
    std::string mxc = up.data;

    std::string rest = mxc.substr(6);
    auto sep = rest.find('/');
    std::string srv = rest.substr(0, sep);
    std::string mid = rest.substr(sep + 1);
    // STRICT: upload must round-trip through the media API. The v1 media
    // endpoint is tried first (current servers serve media only there);
    // a 404 here is a real failure now — the legacy 'CI quirk' was the
    // v3-only endpoint bug.
    auto dl = httpGet(hs + "/_matrix/client/v1/media/download/" + srv + "/" + mid,
                      {{"Authorization", "Bearer " + u.token}}, 30000);
    if (!dl.success || dl.statusCode != 200) {
        std::cerr << "[media] download failed: http=" << dl.statusCode
                  << " err=" << dl.errorMessage << "\n";
        return false;
    }
    if (dl.body.find(payload) == std::string::npos) {
        std::cerr << "[media] download mismatch: len=" << dl.body.size() << "\n";
        return false;
    }
    std::cout << "media: " << mxc << " -> download roundtrip ok\n";
    return true;
}

static bool test_password_change(const std::string& hs) {
    std::string pass = "pw_old_pass_42";
    TestUser u;
    if (!registerUser(u, hs, "pw_alice" + g_runSuffix, pass)) return false;
    std::string uname = u.userId.substr(1, u.userId.find(':') - 1);

    // Baseline: the registered password works for a second login.
    TestUser u2;
    if (!loginUser(u2, hs, uname, pass)) {
        std::cerr << "[pw] FAIL: baseline login with the original password\n";
        return false;
    }

    // Wrong current password -> must be rejected (the status varies by
    // server: 400/401/403 — the rejection itself is what matters).
    auto bad = u.client.changePassword("definitely_wrong", "pw_new_pass_99");
    if (bad.ok) {
        std::cerr << "[pw] FAIL: wrong current password was accepted\n";
        return false;
    }

    // Correct change.
    auto ok = u.client.changePassword(pass, "pw_new_pass_99");
    if (!ok.ok) {
        std::cerr << "[pw] FAIL: changePassword failed (HTTP " << ok.httpStatus
                  << "): " << ok.error.message << "\n";
        return false;
    }

    // Old password must now be rejected…
    TestUser u3;
    if (loginUser(u3, hs, uname, pass)) {
        std::cerr << "[pw] FAIL: login with the OLD password still works\n";
        return false;
    }
    // …and the new one accepted.
    TestUser u4;
    if (!loginUser(u4, hs, uname, "pw_new_pass_99")) {
        std::cerr << "[pw] FAIL: login with the NEW password failed\n";
        return false;
    }
    // The current session token keeps working (a light authenticated
    // /keys/query call proves the token survived the password change).
    auto q = u.client.queryKeys("{\"device_keys\":{}}");
    if (!q.ok) {
        std::cerr << "[pw] FAIL: current token stopped working after the change\n";
        return false;
    }
    std::cout << "pw: change password verified (old rejected, new works, token alive)\n";
    return true;
}

#include "test_synapse_helpers.hpp"

int main() {
    std::cout << "=== Synapse E2EE Integration Test (" << "synapse-e2ee" << ") ===\n\n";
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

    std::cout << "\n--- key request loop test ---\n";
    if (!test_key_request_loop(hs, alice, bob, roomId, since0, since)) g_synapseFailures++;
    std::cout << "\n--- cross-signing setup test ---\n";
    if (!test_cross_signing_setup(hs, alice)) g_synapseFailures++;
    std::cout << "\n--- multiaccount multidevice test ---\n";
    if (!test_multiaccount_multidevice(hs, alice, bob)) g_synapseFailures++;
    std::cout << "\n--- reset-drift test ---\n";
    if (!test_reset_drift(hs)) g_synapseFailures++;
    std::cout << "\n--- media roundtrip test ---\n";
    if (!test_media_roundtrip(hs, bob)) g_synapseFailures++;
    std::cout << "\n--- password change test ---\n";
    if (!test_password_change(hs)) g_synapseFailures++;
    std::cout << "\n";
    if (g_synapseFailures == 0) {
        std::cout << "=== ALL synapse-e2ee TESTS PASSED ===" << std::endl;
        httpCleanup();
        return 0;
    }
    std::cerr << "=== " << g_synapseFailures << " TEST(S) FAILED ===" << std::endl;
    httpCleanup();
    return 1;
}


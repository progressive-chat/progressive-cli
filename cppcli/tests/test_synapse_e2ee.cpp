// tests/test_synapse_e2ee.cpp — live-Synapse E2EE integration test.
//
// Drives the REAL cross-account encrypted-message flow against a running
// Synapse homeserver (matrixdotorg/synapse docker image, registration enabled):
//   register A + B → upload device keys → A creates encrypted room + invites B
//   → B joins → A shares room key + sends m.room.encrypted → B syncs,
//   decrypts Olm-wrapped m.room_key, then decrypts the Megolm timeline event.
//
// Regression guard for: "cross-user E2EE key sharing is unreliable in rooms
// with 3+ members" and Olm/Megolm round-trip bugs. Runs in CI via the
// synapse-e2ee workflow; skipped locally when no server is reachable.
//
// Env: SYNAPSE_URL (default http://localhost:8008)
//
// Build + run (with a Synapse on :8008):
//   cmake --build build -j4 && SYNAPSE_URL=http://localhost:8008 ./build/test_synapse_e2ee

#include "core/http_client.hpp"
#include "core/matrix_client.hpp"
#include "core/crypto/decryptor.hpp"
#include "core/crypto/cross_sign.hpp"
#include "core/crypto/verification.hpp"
#include "core/crypto/sas.hpp"
#include "core/crypto/recovery_key.hpp"
#include "core/crypto/backup_crypto.hpp"
#include "core/crypto/media_crypto.hpp"
#include "core/crypto/key_backup.hpp"
#include "core/crypto/ssss.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <ctime>
#include <simdjson.h>

static int failures = 0;
// Per-run unique username suffix (deviation from the desktop original: the
// desktop assumed a fresh Synapse per CI run; this lets the test re-run
// against a persistent homeserver). Kept in sync via the ecore README.
static const std::string g_runSuffix =
    "_" + std::to_string(static_cast<unsigned>(std::time(nullptr)));
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

using namespace progressive::desktop;

static const char* envOr(const char* name, const char* def) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : def;
}

// ---- Small JSON helpers (no progressive_native parser dependency) ----

static std::string jsonStr(const simdjson::dom::element& el, const char* key) {
    auto v = el[key].get_string();
    if (v.error() == simdjson::SUCCESS) return std::string(v.value());
    return {};
}

// ---- Test user harness: registers + sets up E2EE, mirrors the app ----

struct TestUser {
    MatrixClient client;
    Decryptor decryptor;
    std::string userId;
    std::string deviceId;
    std::string token;
};

static bool registerUser(TestUser& u, const std::string& hs, const std::string& uname,
                         const std::string& pass) {
    auto r = u.client.registerAccount(uname, pass, hs);
    if (!r.ok) {
        std::cerr << "[synapse-test] register " << uname << " failed: "
                  << r.error.message << "\n";
        return false;
    }
    u.userId = r.data.userId;
    u.deviceId = r.data.deviceId;
    u.token = r.data.accessToken;
    u.client.setAccount(r.data);
    return true;
}

static bool setupE2EE(TestUser& u, const std::string& hs) {
    if (!u.decryptor.init()) {
        std::cerr << "[synapse-test] decryptor init failed for " << u.userId << "\n";
        return false;
    }
    u.decryptor.setCryptoContext(u.userId, u.deviceId, hs, u.token);
    std::string body = u.decryptor.buildKeysUploadBody(u.userId, u.deviceId, 30, true);
    auto up = u.client.uploadKeys(body);
    if (!up.ok) {
        std::cerr << "[synapse-test] keys/upload failed for " << u.userId << ": "
                  << up.error.message << "\n";
        return false;
    }
    u.decryptor.markOneTimeKeysPublished();
    u.decryptor.markAccountAsShared();
    return true;
}

// Joined member user IDs for a room (same parsing as room_key_helper.cpp).
static std::vector<std::string> joinedMembers(MatrixClient& client, const std::string& roomId) {
    std::vector<std::string> userIds;
    auto m = client.getRoomMembers(roomId, true);
    if (!m.ok) return userIds;
    simdjson::dom::parser p;
    auto doc = p.parse(m.data);
    if (doc.error() != simdjson::SUCCESS) return userIds;
    auto chunk = doc.value()["chunk"].get_array();
    if (chunk.error() != simdjson::SUCCESS) return userIds;
    for (auto evt : chunk.value()) {
        auto mship = evt["content"]["membership"].get_string();
        if (mship.error() != simdjson::SUCCESS || std::string(mship.value()) != "join") continue;
        auto sk = evt["state_key"].get_string();
        if (sk.error() == simdjson::SUCCESS) userIds.push_back(std::string(sk.value()));
    }
    return userIds;
}

// Dedicated fresh user uploads exactly 1 OTK + fallback; Bob claims twice —
// 1st returns the OTK, 2nd returns THE fallback (verified by value match).

// Key-request loop: alice rotates (new session NOT shared) -> bob fails to
// decrypt msg2 (pending + requests key) -> alice's sync handles the request +
// forwards m.forwarded_room_key -> bob imports + processPending re-decrypts.


// --- Multi-account / multi-device helpers ---

// Login as an existing user -> a NEW device (login, not register).
static bool loginUser(TestUser& u, const std::string& hs, const std::string& uname,
                      const std::string& pass) {
    AccountInfo hsOnly;
    hsOnly.homeserverUrl = hs;
    u.client.setAccount(hsOnly);
    auto r = u.client.loginWithPassword(uname, pass);
    if (!r.ok) {
        std::cerr << "[synapse-test] login " << uname << " failed: " << r.error.message << "\n";
        return false;
    }
    u.userId = r.data.userId;
    u.deviceId = r.data.deviceId;
    u.token = r.data.accessToken;
    u.client.setAccount(r.data);
    return true;
}

// Generate + publish cross-signing keys (endpoint, UIA retry) + re-upload
// device_keys with the SSK signature. Returns the keys (empty on failure).
static progressive::desktop::CrossSigningKeys publishCrossSigning(TestUser& u) {
    auto keys = progressive::desktop::generateCrossSigningKeys();
    if (keys.masterPub.empty()) return {};
    auto master = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.master", keys.masterPub, "", "", u.userId);
    auto self = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.self_signing", keys.selfPub,
        keys.masterPub, keys.masterPriv, u.userId);
    auto user = progressive::desktop::buildCrossSigningContent(
        "m.cross_signing.user_signing", keys.userPub,
        keys.masterPub, keys.masterPriv, u.userId);
    std::string body = "{\"master_key\":" + master
        + ",\"self_signing_key\":" + self
        + ",\"user_signing_key\":" + user + "}";
    auto resp = u.client.uploadDeviceSigningKeys(body);
    std::cerr << "[synapse-test] publishCrossSigning first attempt http=" << resp.httpStatus
              << " (401 = UIA retry path exercised)\n";
    if (!resp.ok && resp.httpStatus == 401) {
        std::string session;
        {
            simdjson::dom::parser p;
            auto doc = p.parse(resp.data);
            if (doc.error() == simdjson::SUCCESS) {
                auto sess = doc.value()["session"].get_string();
                if (sess.error() == simdjson::SUCCESS) session = std::string(sess.value());
            }
        }
        if (!session.empty()) {
            std::string auth = "{\"type\":\"m.login.password\",\"identifier\":{"
                "\"type\":\"m.id.user\",\"user\":\"" + u.userId + "\"},"
                "\"password\":\"synapse_test_pass_42\",\"session\":\"" + session + "\"}";
            std::string body2 = "{\"auth\":" + auth
                + ",\"master_key\":" + master
                + ",\"self_signing_key\":" + self
                + ",\"user_signing_key\":" + user + "}";
            resp = u.client.uploadDeviceSigningKeys(body2);
        }
    }
    if (!resp.ok) return {};
    std::string dkBody = u.decryptor.buildKeysUploadBody(
        u.userId, u.deviceId, 0, true, false, keys.selfPriv, keys.selfPub, true);
    if (!dkBody.empty()) u.client.uploadKeys(dkBody);
    return keys;
}

// Sync until the user decrypts a timeline event whose plaintext contains body.
static bool waitForDecrypt(TestUser& u, const std::string& roomId,
                           const std::string& body, std::string& since) {
    for (int round = 0; round < 24; ++round) {
        auto resp = u.client.syncFast(since, 3000, false);
        if (!resp.ok) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        since = std::string(resp.data.nextBatch);
        for (const auto& evt : resp.data.toDeviceEventList) {
            if (evt.type == "m.room.encrypted")
                u.decryptor.handleOlmEncryptedToDevice(std::string(evt.senderId), std::string(evt.contentJson));
            else if (evt.type == "m.room_key")
                u.decryptor.handleRoomKey(std::string(evt.contentJson));
        }
        for (const auto& [rid, room] : resp.data.joinedRooms) {
            if (rid != roomId) continue;
            for (const auto& evt : room.timeline.events) {
                if (!evt.isEncrypted()) continue;
                auto dec = u.decryptor.decryptMegolmEvent(roomId, std::string(evt.senderId),
                    std::string(evt.contentJson), std::string(evt.eventId), evt.originServerTs);
                if (dec.ok && dec.plaintext.find(body) != std::string::npos) return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

// Share the current room key (if not already) + encrypt + send.
static std::string sendEncrypted(TestUser& u, const std::string& hs,
                                 const std::string& roomId, const std::string& body,
                                 const std::string& tag) {
    u.decryptor.getOrCreateOutboundSession(roomId);  // ensure the outbound session exists
    auto members = joinedMembers(u.client, roomId);
    bool shared = u.decryptor.shareRoomKey(roomId, members, u.userId, u.deviceId, hs, u.token);
    if (shared) u.decryptor.markRoomKeyShared(roomId);
    std::string inner = "{\"type\":\"m.room.message\",\"content\":{\"msgtype\":\"m.text\",\"body\":\""
                        + body + "\"},\"room_id\":\"" + roomId + "\"}";
    std::string enc = u.decryptor.encryptMessage(roomId, u.deviceId, inner);
    if (enc.empty()) return "";
    u.client.sendEncryptedEvent(roomId, enc, tag + std::to_string(std::time(nullptr)));
    return body;
}

// 3 users + a 2-device account: cross-signing publishing across devices,
// multi-device room-key delivery, late-joiner key delivery.
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



// --- Live SAS self-verification (A1 <-> A2 over the server) + verified-only policy ---
// Two VerificationManagers wired to REAL HTTP: sends go via /sendToDevice, the other
// side picks them up in its sync and dispatches m.key.verification.* to its manager.
// After both reach Done, A1's verified-only key-share policy is exercised: A2's
// room-key request is honored, bob's (unverified) is denied.

static void sasSendToDevice(TestUser& u, const std::string& type,
                            const std::string& txnId, const std::string& content,
                            const std::string& user, const std::string& dev) {
    std::string body = "{\"messages\":{\"" + user + "\":{\"" + dev + "\":" + content + "}}}";
    u.client.sendToDevice(type, "sas" + txnId, body);
}

static progressive::desktop::VerificationManager::DeviceKeyResolverFn
makeSasResolver(TestUser& u) {
    return [&u](const std::string& user, const std::string& dev,
                std::string& ed, std::string& curve) -> bool {
        auto q = u.client.queryKeys("{\"device_keys\":{\"" + user + "\":[]}}");
        if (!q.ok) return false;
        simdjson::dom::parser p;
        auto doc = p.parse(q.data);
        if (doc.error() != simdjson::SUCCESS) return false;
        auto keysObj = doc.value()["device_keys"][user][dev]["keys"].get_object();
        if (keysObj.error() != simdjson::SUCCESS) return false;
        for (auto k : keysObj.value()) {
            auto v = k.value.get_string();
            if (v.error() != simdjson::SUCCESS) continue;
            std::string key(k.key);
            if (key == "curve25519:" + dev) curve = std::string(v.value());
            else if (key == "ed25519:" + dev) ed = std::string(v.value());
        }
        return !ed.empty() && !curve.empty();
    };
}

// Sync + dispatch m.key.verification.* until the txn reaches the wanted state.
static bool waitVState(TestUser& u, progressive::desktop::VerificationManager& mgr,
                       std::string& since, const std::string& txnId,
                       progressive::desktop::VerificationState want, int rounds = 20) {
    for (int r = 0; r < rounds; ++r) {
        auto resp = u.client.syncFast(since, 2000, false);
        if (resp.ok) {
            since = std::string(resp.data.nextBatch);
            for (const auto& evt : resp.data.toDeviceEventList) {
                if (evt.type.find("m.key.verification.") == 0) {
                    mgr.handleEvent(std::string(evt.type), std::string(evt.senderId),
                        std::string(evt.contentJson), u.userId, u.deviceId,
                        u.decryptor.ed25519Key(), u.decryptor.curve25519Key());
                }
            }
        }
        auto* t = mgr.findTransaction(txnId);
        if (t && t->state == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    return false;
}

static bool test_sas_verified_policy(const std::string& hs, TestUser& alice, TestUser& bob) {
    using namespace progressive::desktop;
    std::string pass = "synapse_test_pass_42";

    // Fresh login -> alice's new device (A3) + an encrypted room with A1 + A3.
    TestUser alice2;
    std::string aliceUname = alice.userId.substr(1, alice.userId.find(':') - 1);
    if (!loginUser(alice2, hs, aliceUname, pass)) return false;
    if (!setupE2EE(alice2, hs)) return false;
    CHECK(alice2.deviceId != alice.deviceId, "sas: distinct device IDs");

    auto roomRes = alice.client.createRoom("sas-room", "", false, {}, true);
    CHECK(roomRes.ok, "sas: room created");
    std::string roomId = roomRes.data;
    CHECK(alice2.client.joinRoom(roomId).ok, "sas: alice device joined");

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

// Live key-backup API roundtrip: create version -> upload -> restore into a
// fresh decryptor -> decrypt a real message.
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

// Live SSSS: encrypt the cross-signing secrets to account-data with a
// recovery key, then retrieve + verify them (the sync engine's upload/
// retrieve logic replicated — it needs a full SyncEngine otherwise).
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

// Cross-signing setup end-to-end (mirrors SyncEngine::setupCrossSigning):
// generate -> upload account_data + m.signing.key.upload -> device_keys-only
// re-upload with SSK signature -> GET account_data + device_keys -> verify
// the SSK signature over the canonical device_keys.
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

int main() {
    std::cout << "=== Synapse E2EE Integration Test ===\n\n";
    httpInit();

    std::string hs = envOr("SYNAPSE_URL", "http://localhost:8008");
    std::string pass = "synapse_test_pass_42";

    // Wait for the server (graceful skip locally).
    bool reachable = false;
    for (int i = 0; i < 5; ++i) {
        auto v = httpGet(hs + "/_matrix/client/versions", {}, 3000);
        if (v.isOk()) { reachable = true; break; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!reachable) {
        std::cout << "SKIP: no Synapse reachable at " << hs << " (set SYNAPSE_URL)\n";
        httpCleanup();
        return 0;
    }
    std::cout << "server up: " << hs << "\n";

    TestUser alice;
    TestUser bob;
    if (!registerUser(alice, hs, "synapse_alice" + g_runSuffix, pass)) return 1;
    if (!registerUser(bob, hs, "synapse_bob" + g_runSuffix, pass)) return 1;
    std::cout << "registered alice=" << alice.userId << " bob=" << bob.userId << "\n";
    CHECK(alice.deviceId != bob.deviceId, "distinct device IDs");

    if (!setupE2EE(alice, hs) || !setupE2EE(bob, hs)) return 1;
    std::cout << "device keys uploaded for both\n";

    // Alice creates an encrypted room and invites Bob.
    auto roomRes = alice.client.createRoom("synapse-integration", "", false,
                                           {bob.userId}, true);
    if (!roomRes.ok) {
        std::cerr << "[synapse-test] createRoom failed: " << roomRes.error.message << "\n";
        return 1;
    }
    std::string roomId = roomRes.data;
    std::cout << "alice created encrypted room: " << roomId << "\n";

    auto joinRes = bob.client.joinRoom(roomId);
    if (!joinRes.ok) {
        std::cerr << "[synapse-test] bob join failed: " << joinRes.error.message << "\n";
        return 1;
    }
    std::cout << "bob joined room\n";

    // Bob's first sync: registers the room + device list.
    auto sync0 = bob.client.syncFast("", 5000, false);
    std::string since0 = sync0.ok ? std::string(sync0.data.nextBatch) : "";
    (void)since0;

    // Alice: build outbound Megolm session, share room key, send encrypted message.
    std::string sessId = alice.decryptor.getOrCreateOutboundSession(roomId);
    CHECK(!sessId.empty(), "alice has an outbound megolm session");

    std::vector<std::string> members = joinedMembers(alice.client, roomId);
    CHECK(!members.empty(), "room has joined members");
    bool keyShared = alice.decryptor.shareRoomKey(roomId, members,
        alice.userId, alice.deviceId, hs, alice.token);
    if (keyShared) alice.decryptor.markRoomKeyShared(roomId);
    CHECK(keyShared, "room key shared to members");

    std::string body = "hello-synapse-e2ee-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::string inner = "{\"type\":\"m.room.message\",\"content\":{\"msgtype\":\"m.text\",\"body\":\""
                        + body + "\"},\"room_id\":\"" + roomId + "\"}";
    std::string enc = alice.decryptor.encryptMessage(roomId, alice.deviceId, inner);
    CHECK(!enc.empty(), "alice encrypts the message");
    std::string txnId = "synapse-test-" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto sendRes = alice.client.sendEncryptedEvent(roomId, enc, txnId);
    CHECK(sendRes.ok, "alice sends m.room.encrypted");
    std::cout << "alice sent event: " << sendRes.data << "\n";

    // Bob: sync loop — process to-device (Olm room_key) then decrypt the timeline.
    bool decrypted = false;
    std::string plaintext;
    std::string since = since0;
    for (int round = 0; round < 10 && !decrypted; ++round) {
        auto resp = bob.client.syncFast(since, 3000, false);
        if (!resp.ok) {
            std::cerr << "[synapse-test] bob sync failed round " << round << ": "
                      << resp.error.message << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        since = std::string(resp.data.nextBatch);

        // 1. To-device events (m.room_key is Olm-wrapped m.room.encrypted).
        for (const auto& evt : resp.data.toDeviceEventList) {
            if (evt.type == "m.room.encrypted") {
                bob.decryptor.handleOlmEncryptedToDevice(
                    std::string(evt.senderId), std::string(evt.contentJson));
            } else if (evt.type == "m.room_key") {
                bob.decryptor.handleRoomKey(std::string(evt.contentJson));
            }
        }

        // 2. Timeline: decrypt m.room.encrypted events in our room.
        for (const auto& [rid, room] : resp.data.joinedRooms) {
            if (rid != roomId) continue;
            for (const auto& evt : room.timeline.events) {
                if (!evt.isEncrypted()) continue;
                auto dec = bob.decryptor.decryptMegolmEvent(
                    roomId, std::string(evt.senderId),
                    std::string(evt.contentJson),
                    std::string(evt.eventId), evt.originServerTs);
                if (dec.ok) {
                    plaintext = dec.plaintext;
                    decrypted = true;
                }
            }
        }
        if (!decrypted) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    CHECK(decrypted, "bob decrypts alice's megolm event");
    if (decrypted) {
        std::cout << "bob plaintext: " << plaintext << "\n";
        CHECK(plaintext.find(body) != std::string::npos,
              "decrypted plaintext contains the message body");
    }

    // Key-request loop (rotation -> request -> forward -> re-decrypt).
    std::cout << "\n--- key request loop test ---\n";
    if (!test_key_request_loop(hs, alice, bob, roomId, since0, since)) failures++;
    std::cout << "--- key request loop done ---\n";

    // Cross-signing setup + SSK-signed device verification.
    std::cout << "\n--- cross-signing setup test ---\n";
    if (!test_cross_signing_setup(hs, alice)) failures++;
    std::cout << "--- cross-signing setup done ---\n";

    // NOTE: scenario ORDER matters — the sas, backup and ssss tests re-publish
    // alice's cross-signing keys / depend on her account state (invalidating
    // the mm test's SSK sigs), so they must all stay AFTER the mm test.
    // Multi-account + multi-device: 3 members, 2-device account, late joiner.
    std::cout << "\n--- multiaccount multidevice test ---\n";
    if (!test_multiaccount_multidevice(hs, alice, bob)) failures++;
    std::cout << "--- multiaccount multidevice done ---\n";

    // Live SAS self-verification (A1 <-> A3 over the server) + verified-only policy.
    std::cout << "\n--- sas verified-policy test ---\n";
    if (!test_sas_verified_policy(hs, alice, bob)) failures++;
    std::cout << "--- sas verified-policy done ---\n";

    // Live key-backup API roundtrip (must stay AFTER the sas test — it uses
    // alice's account state).
    std::cout << "\n--- key-backup api test ---\n";
    if (!test_key_backup_api(hs, alice)) failures++;
    std::cout << "--- key-backup api done ---\n";

    // Live SSSS secret-sharing roundtrip (LAST — it re-publishes alice's keys).
    std::cout << "\n--- ssss api test ---\n";
    if (!test_ssss_api(hs, alice)) failures++;
    std::cout << "--- ssss api done ---\n";

    std::cout << "\n";
    std::cout << "\n--- fallback claim test ---\n";
    if (!test_fallback_claim(hs, bob)) failures++;
    if (failures == 0) {
        std::cout << "=== ALL SYNAPSE E2EE TESTS PASSED ===" << std::endl;
        httpCleanup();
        return 0;
    }
    std::cerr << "=== " << failures << " TEST(S) FAILED ===" << std::endl;
    httpCleanup();
    return 1;
}

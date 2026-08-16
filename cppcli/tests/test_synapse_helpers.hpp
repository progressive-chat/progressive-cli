// test_synapse_helpers.hpp — the shared harness for the synapse E2EE
// integration tests (split across two binaries).
#pragma once


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
#include <fstream>
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#include <ctime>
#include <simdjson.h>

// The shared failure counter + CHECK (per binary).
static int g_synapseFailures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "  [FAIL] " << msg << " (" << __LINE__ << ")"        \
                      << std::endl;                                           \
            g_synapseFailures++;                                              \
        }                                                                     \
    } while (0)


// Per-run unique username suffix (deviation from the desktop original: the
// desktop assumed a fresh Synapse per CI run; this lets the test re-run
// against a persistent homeserver). Kept in sync via the ecore README.
static const std::string g_runSuffix =
    "_" + std::to_string(static_cast<unsigned>(std::time(nullptr)));
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "  [FAIL] " << msg << " (" << __LINE__ << ")"        \
                      << std::endl;                                           \
            g_synapseFailures++;                                              \
        }                                                                     \
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

// --- Reset-drift regression ---
// The receiver must not be stranded when the sender resets their identity:
// dave stores the megolm session under carol's CURRENT curve25519 key; carol
// then calls resetIdentity() (new identity keys, so the next outbound
// session has a NEW sender key + session id). Carol's next message must
// still decrypt for dave — this is the "sender_key drift" bug: the store
// was keyed by the event's sender_key, and the strict (room, sender_key,
// session_id) lookup missed after the reset (fallback by (room, session_id)
// + outbound discard/rotation keep the receiver working).
// The basic flow every binary repeats: register A+B, E2EE setup, A creates
// an encrypted room + invites B, B joins, A shares the room key and sends
// an encrypted message, B decrypts it. Returns false on a hard failure.
static bool runBasicFlow(const std::string& hs, const std::string& pass,
                         TestUser& alice, TestUser& bob,
                         std::string& roomId, std::string& since0,
                         std::string& since) {
    if (!registerUser(alice, hs, "synapse_alice" + g_runSuffix, pass)) return false;
    if (!registerUser(bob, hs, "synapse_bob" + g_runSuffix, pass)) return false;
    std::cout << "registered alice=" << alice.userId << " bob=" << bob.userId << "\n";
    CHECK(alice.deviceId != bob.deviceId, "distinct device IDs");
    if (!setupE2EE(alice, hs) || !setupE2EE(bob, hs)) return false;

    auto roomRes = alice.client.createRoom("synapse-integration", "", false,
                                           {bob.userId}, true);
    if (!roomRes.ok) {
        std::cerr << "[synapse-test] createRoom failed: " << roomRes.error.message << "\n";
        return false;
    }
    roomId = roomRes.data;
    if (!bob.client.joinRoom(roomId).ok) {
        std::cerr << "[synapse-test] bob join failed\n";
        return false;
    }
    auto sync0 = bob.client.syncFast("", 5000, false);
    since0 = sync0.ok ? std::string(sync0.data.nextBatch) : "";

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
    CHECK(alice.client.sendEncryptedEvent(roomId, enc, txnId).ok,
          "alice sends m.room.encrypted");

    bool decrypted = false;
    std::string plaintext;
    since = since0;
    for (int round = 0; round < 10 && !decrypted; ++round) {
        auto resp = bob.client.syncFast(since, 3000, false);
        if (!resp.ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        since = std::string(resp.data.nextBatch);
        for (const auto& evt : resp.data.toDeviceEventList) {
            if (evt.type == "m.room.encrypted") {
                bob.decryptor.handleOlmEncryptedToDevice(
                    std::string(evt.senderId), std::string(evt.contentJson));
            } else if (evt.type == "m.room_key") {
                bob.decryptor.handleRoomKey(std::string(evt.contentJson));
            }
        }
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
        CHECK(plaintext.find(body) != std::string::npos,
              "decrypted plaintext contains the message body");
    }
    return true;
}

// The shared server-up probe (graceful skip when no Synapse is reachable).
static bool serverUp(const std::string& hs) {
    for (int i = 0; i < 5; ++i) {
        auto v = httpGet(hs + "/_matrix/client/versions", {}, 3000);
        if (v.isOk()) return true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

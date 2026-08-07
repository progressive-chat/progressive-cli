// src/core/crypto/decryptor.cpp — E2EE coordinator (Olm + Megolm).

#include "decryptor.hpp"
#include "olm_account.hpp"
#include "random.hpp"
#include "sig_verify.hpp"
#include "cross_sign.hpp"

#include <progressive/olm.hpp>
#include <olm/olm.h>
#include <olm/outbound_group_session.h>

#include "../http_client.hpp"
#include "../debug_log.hpp"
#include <simdjson.h>
#include <string_view>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <vector>
#include <map>
#include <random>
#include <atomic>
#include <chrono>

namespace progressive::desktop {

// Reused outbound Olm sessions, keyed userId|deviceId (see sendOlmToDevice).
struct Decryptor::OutboundOlmTarget {
    std::unique_ptr<progressive::OlmSession> session;
    std::string curve;
};

static std::atomic<uint64_t> g_txnCounter{0};

Decryptor::Decryptor()
    : account_(std::make_unique<OlmAccountStore>()),
      megolm_(std::make_unique<MegolmStore>()) {}

Decryptor::~Decryptor() = default;

// Drop everything that is scoped to the previous account: requests, room-key
// notifications, recovery notes, stale-device marks, broken-Olm state, and
// pending re-decrypted events. Called on every init (login, switch, restart).
void Decryptor::clearPerAccountState() {
    {
        std::lock_guard<std::mutex> lk(olmMtx_);
        olmSessions_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(outboundOlmMtx_);
        outboundOlmSessions_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(outboundMtx_);
        outboundSessions_.clear();
        roomKeysShared_.clear();
        roomEncryptionConfigs_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        requestedKeys_.clear();
        recentKeyRequests_.clear();
        forcedOlm_.clear();
        lastRequestGateMs_ = 0;
        requestGateCount_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(roomKeyNotifMtx_);
        roomKeyNotifications_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(olmRecoveryNoteMtx_);
        lastOlmRecoveryNote_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(brokenOlmMtx_);
        brokenOlmSenders_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(staleMtx_);
        staleDeviceUsers_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(reDecryptedMtx_);
        reDecryptedEvents_.clear();
    }
}

bool Decryptor::init(const std::string& accountPickle, const std::string& pickleKey,
                      bool shared) {
    clearPerAccountState();
    if (!account_->reset()) return false;  // fresh libolm memory — never load over an initialized account
    if (!accountPickle.empty()) {
        if (!account_->load(accountPickle, pickleKey)) {
            return account_->create();
        }
        account_->setShared(shared);
        return true;
    }
    return account_->create();
}

bool Decryptor::init() {
    clearPerAccountState();
    if (!account_->reset()) return false;
    return account_->create();
}

std::string Decryptor::saveAccountPickle(const std::string& pickleKey) {
    return account_->save(pickleKey);
}

OlmIdentityKeys Decryptor::identityKeys() const {
    return account_->identityKeys();
}

std::string Decryptor::curve25519Key() const {
    return account_->curve25519Key();
}

std::string Decryptor::ed25519Key() const {
    return account_->ed25519Key();
}

void Decryptor::markDevicesStale(const std::vector<std::string>& userIds) {
    std::lock_guard<std::mutex> lk(staleMtx_);
    for (const auto& uid : userIds) {
        if (staleDeviceUsers_.size() >= 1000) {
            static bool warned = false;
            if (!warned) {
                LOG(LogChannel::E2EE, "markDevicesStale: cap 1000 reached, dropping further entries");
                warned = true;
            }
            break;
        }
        staleDeviceUsers_.insert(uid);
    }
}

bool Decryptor::isDeviceStale(const std::string& userId) {
    std::lock_guard<std::mutex> lk(staleMtx_);
    return staleDeviceUsers_.count(userId) > 0;
}

void Decryptor::clearStale(const std::string& userId) {
    std::lock_guard<std::mutex> lk(staleMtx_);
    staleDeviceUsers_.erase(userId);
}

DecryptionResult Decryptor::decryptMegolmEvent(const std::string& roomId,
                                                  const std::string& senderId,
                                                  const std::string& contentJson,
                                                  const std::string& eventId,
                                                  int64_t originServerTs) {
    DecryptionResult r;
    simdjson::dom::parser mp;
    auto doc = mp.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) {
        r.error = "failed to parse megolm encrypted content";
        return r;
    }
    auto val = doc.value();
    auto algoStr = val["algorithm"].get_string();
    if (algoStr.error() != simdjson::SUCCESS) {
        r.error = "missing algorithm";
        return r;
    }
    std::string algorithm(algoStr.value());
    if (algorithm != "m.megolm.v1.aes-sha2" && algorithm != "m.megolm.v2.aes-sha2") {
        r.error = "unsupported algorithm: " + algorithm;
        return r;
    }
    auto sk = val["sender_key"].get_string();
    auto sid = val["session_id"].get_string();
    auto ct = val["ciphertext"].get_string();
    auto devId = val["device_id"].get_string();
    if (sk.error() != simdjson::SUCCESS || sid.error() != simdjson::SUCCESS ||
        ct.error() != simdjson::SUCCESS) {
        r.error = "missing sender_key/session_id/ciphertext";
        return r;
    }
    std::string senderKey(sk.value());
    std::string sessionId(sid.value());
    std::string ciphertext(ct.value());
    std::string senderDeviceId = (devId.error() == simdjson::SUCCESS)
        ? std::string(devId.value()) : "";

    if (!megolm_->hasSession(roomId, senderKey, sessionId)) {
        r.error = enrichDecryptError(senderKey, "no megolm session — waiting for room_key");
        LOG(LogChannel::E2EE, "decryptMegolmEvent: no session room=%.40s eid=%s sid=%.30s senderKey=%.30s devId=%s — saving to pending",
            roomId.c_str(), eventId.c_str(), sessionId.c_str(), senderKey.c_str(),
            senderDeviceId.c_str());
        PendingEncryptedEvent p;
        p.roomId = roomId;
        p.senderKey = senderKey;
        p.sessionId = sessionId;
        p.ciphertext = ciphertext;
        p.senderId = senderId;
        p.eventId = eventId;
        p.originServerTs = originServerTs;
        megolm_->addPending(p);
        requestRoomKey(roomId, senderId, senderKey, sessionId, senderDeviceId);
        return r;
    }

    auto plaintext = megolm_->decrypt(roomId, senderKey, sessionId, ciphertext);
    if (plaintext.empty()) {
        r.error = enrichDecryptError(senderKey, "megolmDecrypt failed (bad mac or unknown session)");
        return r;
    }
    r.ok = true;
    r.plaintext = std::move(plaintext);
    return r;
}

bool Decryptor::handleRoomKey(const std::string& contentJson,
    const std::string& senderId) {
    {
        // A received session satisfies any pending requests for it.
        simdjson::dom::parser p;
        auto doc = p.parse(contentJson);
        if (doc.error() == simdjson::SUCCESS) {
            auto rid = doc.value()["room_id"].get_string();
            auto sid = doc.value()["session_id"].get_string();
            auto sk = doc.value()["sender_key"].get_string();
            if (rid.error() == simdjson::SUCCESS && sid.error() == simdjson::SUCCESS &&
                sk.error() == simdjson::SUCCESS) {
                std::string key = std::string(rid.value()) + "|" +
                                  std::string(sid.value()) + "|" +
                                  std::string(sk.value());
                KeyRequestState st;
                bool hadRequest = false;
                {
                    std::lock_guard<std::mutex> lk(requestMtx_);
                    auto it = requestedKeys_.find(key);
                    if (it != requestedKeys_.end()) {
                        st = it->second;
                        hadRequest = true;
                        requestedKeys_.erase(it);
                    }
                }
                if (hadRequest) sendRequestCancellation(st);
            }
        }
    }
    simdjson::dom::parser rp;
    auto doc = rp.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) return false;
    auto val = doc.value();

    auto rid = val["room_id"].get_string();
    auto sid = val["session_id"].get_string();
    auto skey = val["session_key"].get_string();
    if (rid.error() != simdjson::SUCCESS || sid.error() != simdjson::SUCCESS ||
        skey.error() != simdjson::SUCCESS) {
        LOG(LogChannel::E2EE, "handleRoomKey: FAILED — missing required fields");
        return false;
    }
    std::string roomId(rid.value());
    std::string sessionId(sid.value());
    std::string sessionKey(skey.value());

    auto sk = val["sender_key"].get_string();
    std::string senderKey;
    if (sk.error() == simdjson::SUCCESS) {
        senderKey = std::string(sk.value());
    } else {
        auto keys = val["keys"].get_object();
        if (keys.error() == simdjson::SUCCESS) {
            for (auto [k, v] : keys.value()) {
                std::string kStr(k);
                if (kStr.find("curve25519") != std::string::npos) {
                    auto kv = v.get_string();
                    if (kv.error() == simdjson::SUCCESS) senderKey = std::string(kv.value());
                    break;
                }
            }
        }
    }
    LOG(LogChannel::E2EE, "handleRoomKey: room=%.40s sid=%.20s sk=%.20s senderKey=%.20s",
        roomId.c_str(), sessionId.c_str(), sessionKey.c_str(), senderKey.c_str());
    if (senderKey.empty()) {
        LOG(LogChannel::E2EE, "handleRoomKey: FAILED — no sender_key");
        return false;
    }
    bool ok = megolm_->addInboundSession(roomId, senderKey, sessionId, sessionKey);
    LOG(LogChannel::E2EE, "handleRoomKey: addInboundSession=%d", ok ? 1 : 0);
    if (ok) {
        noteRoomKey({roomId, sessionId, senderId, RoomKeyEventKind::Received, 0, 0});
        processPending(roomId, senderKey, sessionId);
    }
    return ok;
}

void Decryptor::processPending(const std::string& roomId,
                               const std::string& senderKey,
                               const std::string& sessionId) {
    auto pending = megolm_->takePendingForSession(roomId, senderKey, sessionId);
    LOG(LogChannel::E2EE, "processPending: %zu pending events for room=%.40s",
        pending.size(), roomId.c_str());
    if (pending.empty()) {
        std::fprintf(stderr, "[E2EE] processPending: NO MATCH for room=%.40s sid=%.20s sk=%.20s\n",
            roomId.c_str(), sessionId.c_str(), senderKey.c_str());
        return;
    }

    std::lock_guard<std::mutex> lk(reDecryptedMtx_);
    for (const auto& p : pending) {
        auto plaintext = megolm_->decrypt(p.roomId, p.senderKey, p.sessionId, p.ciphertext);
        if (!plaintext.empty()) {
            LOG(LogChannel::E2EE, "processPending: DECRYPTED eid=%s", p.eventId.c_str());
        } else {
            LOG(LogChannel::E2EE, "processPending: FAILED eid=%s", p.eventId.c_str());
            continue;
        }
        ReDecryptedEvent evt;
        evt.roomId = p.roomId;
        evt.eventId = p.eventId;
        evt.plaintext = std::move(plaintext);
        evt.senderId = p.senderId;
        evt.originServerTs = p.originServerTs;
        reDecryptedEvents_.push_back(std::move(evt));
    }
}

std::vector<ReDecryptedEvent> Decryptor::takeDecryptedEvents() {
    std::lock_guard<std::mutex> lk(reDecryptedMtx_);
    std::vector<ReDecryptedEvent> out;
    out.swap(reDecryptedEvents_);
    return out;
}

std::vector<RoomKeyNotification> Decryptor::takeRoomKeyNotifications() {
    std::lock_guard<std::mutex> lk(roomKeyNotifMtx_);
    std::vector<RoomKeyNotification> out;
    out.swap(roomKeyNotifications_);
    return out;
}

void Decryptor::noteOlmRecovery(const std::string& senderId, const std::string& senderKey) {
    std::lock_guard<std::mutex> lk(olmRecoveryNoteMtx_);
    lastOlmRecoveryNote_ = senderId + " (device " + senderKey.substr(0, 8) + "…)" +
        " — Olm session was broken; re-established. If messages stay encrypted, " +
        "ask them to reset encryption in Element.";
}

std::string Decryptor::takeLastOlmRecoveryNote() {
    std::lock_guard<std::mutex> lk(olmRecoveryNoteMtx_);
    std::string out = std::move(lastOlmRecoveryNote_);
    lastOlmRecoveryNote_.clear();
    return out;
}

void Decryptor::markOlmBroken(const std::string& senderKey) {
    if (senderKey.empty()) return;
    std::lock_guard<std::mutex> lk(brokenOlmMtx_);
    brokenOlmSenders_[senderKey] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Decryptor::enrichDecryptError(const std::string& senderKey,
                                          const std::string& baseError) const {
    if (baseError.empty()) return baseError;
    {
        std::lock_guard<std::mutex> lk(brokenOlmMtx_);
        auto it = brokenOlmSenders_.find(senderKey);
        if (it == brokenOlmSenders_.end()) return baseError;
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - it->second > 30 * 60 * 1000) return baseError;
    }
    return baseError + " — The sender's Olm session with you is broken; they can't "
        "deliver the room key, so only they can see this message right now. Ask them "
        "to restart their client or reset its encryption in Element, or run "
        "Settings \u2192 Reset device keys here.";
}

void Decryptor::noteRoomKey(RoomKeyNotification n) {
    n.ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(roomKeyNotifMtx_);
    roomKeyNotifications_.push_back(std::move(n));
}

// A sender refused to give us a room key (m.room_key.withheld). The event
// carries no room/session id — match it against our outstanding key requests
// (sender_key + device) to surface "X withheld the key: <reason>" in the
// right room.
void Decryptor::noteWithheld(const std::string& senderKey,
                             const std::string& fromDevice,
                             const std::string& reason) {
    std::lock_guard<std::mutex> lk(requestMtx_);
    for (const auto& [key, st] : requestedKeys_) {
        auto sep1 = key.find('|');
        auto sep2 = key.find('|', sep1 == std::string::npos ? 0 : sep1 + 1);
        if (sep1 == std::string::npos || sep2 == std::string::npos) continue;
        const std::string roomId = key.substr(0, sep1);
        const std::string sessionId = key.substr(sep1 + 1, sep2 - sep1 - 1);
        const std::string sk = key.substr(sep2 + 1);
        bool deviceMatches = fromDevice.empty() || fromDevice == st.senderDeviceId ||
                             fromDevice == "*";
        if (sk == senderKey && deviceMatches) {
            noteRoomKey({roomId, sessionId, st.senderId,
                         RoomKeyEventKind::Withheld, 0, 0, reason});
            return;
        }
    }
    LOG(LogChannel::E2EE, "withheld: no matching pending request for senderKey=%.20s "
        "(code=m.no_olm — sender could not establish a secure channel)", senderKey.c_str());
}

// ---- Device key upload body builder ----

std::string Decryptor::signCanonicalJson(const std::string& canonicalJson) {
    return account_->sign(canonicalJson);
}

std::string Decryptor::buildKeysUploadBody(const std::string& userId,
                                               const std::string& deviceId,
                                               int oneTimeKeyCount,
                                               bool includeDeviceKeys,
                                               bool includeFallbackKey,
                                               const std::string& sskPrivB64,
                                               const std::string& sskPubB64,
                                               bool omitOneTimeKeys) {
    // 1. Generate one-time keys (skipped for device_keys-only re-uploads —
    //    emitting an empty one_time_keys section would WIPE the server pool).
    std::string oneTimeKeysJson;
    if (!omitOneTimeKeys) oneTimeKeysJson = account_->generateOneTimeKeys(oneTimeKeyCount);

    // 2. Build device_keys object with sorted keys (canonical JSON).
    auto keys = account_->identityKeys();
    // The device_keys JSON (without signatures):
    // {"algorithms":[...],"device_id":"...","keys":{...},"user_id":"..."}
    std::ostringstream dk;
    dk << "{\"algorithms\":[\"m.olm.v1.curve25519-aes-sha2\",\"m.megolm.v1.aes-sha2\"],"
       << "\"device_id\":\"" << deviceId << "\","
       << "\"keys\":{"
       << "\"curve25519:" << deviceId << "\":\"" << keys.curve25519 << "\","
       << "\"ed25519:" << deviceId << "\":\"" << keys.ed25519 << "\""
       << "},"
       << "\"user_id\":\"" << userId << "\""
       << "}";
    std::string deviceKeysCanonical = dk.str();

    // 3. Sign the device_keys canonical JSON
    std::string signature = signCanonicalJson(deviceKeysCanonical);

    // Insert signatures into device_keys before the closing }
    std::string deviceKeysSigned = deviceKeysCanonical;
    deviceKeysSigned.pop_back();  // remove trailing }
    std::string signatures = "{\"ed25519:" + deviceId + "\":\"" + signature + "\"";
    if (!sskPrivB64.empty() && !sskPubB64.empty()) {
        // Cross-sign the device with our SSK (signatures[userId]["ed25519:<sskPub>"]).
        std::string sskSig = signEd25519(sskPrivB64, deviceKeysCanonical);
        if (!sskSig.empty())
            signatures += ",\"ed25519:" + sskPubB64 + "\":\"" + sskSig + "\"";
    }
    signatures += "}";
    deviceKeysSigned += ",\"signatures\":{\"" + userId + "\":" + signatures + "}}";

    // 4. Parse the one-time keys JSON and sign each one.
    // The oneTimeKeysJson from progressive::OlmAccount looks like:
    //   {"curve25519:AAAA":"<key>","curve25519:BBBB":"<key>"}
    // We need to:
    //   a) Rename "curve25519:" prefix to "signed_curve25519:"
    //   b) Sign each key object ({"key":"<value>"}) and add signature
    // For simplicity, we use simdjson to parse and re-build the signed format.
    std::ostringstream otkSigned;
    otkSigned << "{";
    bool firstOtk = true;
    simdjson::dom::parser parser;
    auto otkResult = parser.parse(oneTimeKeysJson);
    if (otkResult.error() == simdjson::SUCCESS) {
        auto obj = otkResult.value().get_object();
        if (obj.error() == simdjson::SUCCESS) {
            for (auto field : obj.value()) {
                std::string_view key(field.key);

                auto innerObj = field.value.get_object();
                if (innerObj.error() == simdjson::SUCCESS) {
                    // Nested format: {"curve25519":{"AAAAqg":"<key>","AAAAqQ":"<key>"}}
                    // This is what libolm returns since the JSON retrieval fix.
                    for (auto innerField : innerObj.value()) {
                        std::string innerKey(innerField.key);
                        auto innerVal = innerField.value.get_string();
                        if (innerVal.error() != simdjson::SUCCESS) continue;

                        std::string keyObj = "{\"key\":\"" + std::string(innerVal.value()) + "\"}";
                        std::string sig = signCanonicalJson(keyObj);
                        std::string signedKey = "signed_curve25519:" + innerKey;
                        if (!firstOtk) otkSigned << ",";
                        firstOtk = false;
                        otkSigned << "\"" << signedKey << "\":"
                                  << "{\"key\":\"" << std::string(innerVal.value()) << "\","
                                  << "\"signatures\":{\""
                                  << userId << "\":{\"ed25519:" << deviceId << "\":\"" << sig << "\"}}}";
                    }
                } else {
                    // Legacy flat format: {"curve25519:AAAAqg":"<key>","curve25519:BBBB":"<key>"}
                    auto valStr = field.value.get_string();
                    if (valStr.error() != simdjson::SUCCESS) continue;

                    std::string keyObj = "{\"key\":\"" + std::string(valStr.value()) + "\"}";
                    std::string sig = signCanonicalJson(keyObj);
                    std::string signedKey = "signed_curve25519:" + std::string(key).substr(key.find(':') + 1);
                    if (!firstOtk) otkSigned << ",";
                    firstOtk = false;
                    otkSigned << "\"" << signedKey << "\":"
                              << "{\"key\":\"" << std::string(valStr.value()) << "\","
                              << "\"signatures\":{\""
                              << userId << "\":{\"ed25519:" << deviceId << "\":\"" << sig << "\"}}}";
                }
            }
        }
    }
    otkSigned << "}";

    // 5. Assemble the full /keys/upload body
    // Assemble sections with proper comma joining (device_keys-only uploads
    // must not leave a trailing comma).
    std::vector<std::string> sections;
    if (includeDeviceKeys) sections.push_back("\"device_keys\":" + deviceKeysSigned);
    if (includeFallbackKey) {
        std::string fallbackSection = buildFallbackKeysSection(userId, deviceId);
        if (!fallbackSection.empty())
            sections.push_back("\"fallback_keys\":" + fallbackSection);
    }
    if (!omitOneTimeKeys) sections.push_back("\"one_time_keys\":" + otkSigned.str());

    std::ostringstream body;
    body << "{";
    for (size_t i = 0; i < sections.size(); ++i) {
        if (i > 0) body << ",";
        body << sections[i];
    }
    body << "}";
    return body.str();
}

std::string Decryptor::buildFallbackKeysSection(const std::string& userId,
                                                const std::string& deviceId) {
    // The unpublished fallback key from libolm looks like:
    //   {"curve25519":{"AAAA":"<b64>"}}
    // We rename "curve25519:" to "signed_curve25519:" and sign it like an OTK.
    std::string fallbackJson = account_->unpublishedFallbackKey();
    if (fallbackJson.empty()) {
        if (!account_->generateFallbackKey()) return {};
        fallbackJson = account_->unpublishedFallbackKey();
        if (fallbackJson.empty()) return {};
    }

    simdjson::dom::parser parser;
    auto doc = parser.parse(fallbackJson);
    if (doc.error() != simdjson::SUCCESS) return {};

    std::ostringstream out;
    out << "{";
    bool first = true;
    auto root = doc.value().get_object();
    if (root.error() == simdjson::SUCCESS) {
        for (auto field : root.value()) {
            auto innerObj = field.value.get_object();
            if (innerObj.error() != simdjson::SUCCESS) continue;
            for (auto innerField : innerObj.value()) {
                std::string keyId(innerField.key);
                auto keyVal = innerField.value.get_string();
                if (keyVal.error() != simdjson::SUCCESS) continue;

                std::string keyObj = "{\"key\":\"" + std::string(keyVal.value()) + "\"}";
                std::string sig = signCanonicalJson(keyObj);
                if (!first) out << ",";
                first = false;
                out << "\"signed_curve25519:" << keyId << "\":"
                    << "{\"key\":\"" << std::string(keyVal.value()) << "\","
                    << "\"signatures\":{\""
                    << userId << "\":{\"ed25519:" << deviceId << "\":\"" << sig << "\"}}}";
            }
        }
    }
    out << "}";
    if (first) return {};  // nothing found
    return out.str();
}

void Decryptor::markOneTimeKeysPublished() {
    if (account_) account_->markOneTimeKeysPublished();
}

// ---- Olm 1:1 inbound session management ----

std::string Decryptor::handleOlmEncryptedToDevice(const std::string& senderId,
                                                       const std::string& contentJson) {
    // m.room.encrypted to-device content (Olm 1:1):
    //   {"algorithm":"m.olm.v1.curve25519-aes-sha2","ciphertext":
    //    {"<our_curve25519>":{"body":"<base64>","type":0}},"sender_key":"<their_curve25519>"}
    simdjson::dom::parser op;
    auto doc = op.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) return {};
    auto val = doc.value();

    auto algoStr = val["algorithm"].get_string();
    if (algoStr.error() != simdjson::SUCCESS ||
        std::string(algoStr.value()) != "m.olm.v1.curve25519-aes-sha2") {
        LOG(LogChannel::E2EE, "Olm: wrong algorithm");
        return {};
    }

    auto sk = val["sender_key"].get_string();
    if (sk.error() != simdjson::SUCCESS) {
        LOG(LogChannel::E2EE, "Olm: no sender_key");
        return {};
    }
    std::string senderKey(sk.value());

    std::string ourCurve = account_->curve25519Key();
    if (ourCurve.empty()) {
        LOG(LogChannel::E2EE, "Olm: no our curve25519 key");
        return {};
    }

    auto ct = val["ciphertext"];
    if (ct.error() != simdjson::SUCCESS) {
        LOG(LogChannel::E2EE, "Olm: no ciphertext");
        return {};
    }
    auto ourEntry = ct.value()[ourCurve];
    if (ourEntry.error() != simdjson::SUCCESS) {
        LOG(LogChannel::E2EE, "Olm: our key not found in ciphertext");
        LOG(LogChannel::E2EE, "Olm: our curve25519=%s", ourCurve.c_str());
        auto ctObj = ct.value().get_object();
        if (ctObj.error() == simdjson::SUCCESS) {
            for (auto entry : ctObj.value()) {
                LOG(LogChannel::E2EE, "Olm: ciphertext has key=%s",
                    std::string(entry.key).c_str());
                LOG(LogChannel::E2EE, "IDENTITY-HINT: sender encrypted to %s — a key we "
                    "no longer hold. Their client/server still cache an OLD identity of "
                    "ours (after a device-key reset). They must refresh their device "
                    "list or reset encryption in their client.",
                    std::string(entry.key).c_str());
            }
        }
        return {};
    }

    auto bodyStr = ourEntry.value()["body"].get_string();
    auto typeNum = ourEntry.value()["type"].get_int64();
    if (bodyStr.error() != simdjson::SUCCESS) {
        LOG(LogChannel::E2EE, "Olm: empty body");
        return {};
    }
    std::string body(bodyStr.value());
    std::fprintf(stderr, "[E2EE] OLMBODY: %s\n", body.c_str());
    int msgType = (typeNum.error() == simdjson::SUCCESS) ? static_cast<int>(typeNum.value()) : 0;

    std::fprintf(stderr, "[E2EE] Olm cipherObj: parsed via simdjson\n");
    std::fprintf(stderr, "[E2EE] DBG1: body size=%zu\n", body.size());
    std::fprintf(stderr, "[E2EE] DBG2: body size=%zu empty=%d\n", body.size(), body.empty() ? 1 : 0);
    std::fprintf(stderr, "[E2EE] DBG3: typeStr='%d'\n", msgType);
    std::fprintf(stderr, "[E2EE] DBG4: msgType=%d\n", msgType);

    if (body.empty()) {
        LOG(LogChannel::E2EE, "Olm: empty body in ciphertext object");
        return {};
    }

    // Try to find an existing OlmSession for this sender.
    // If none, create one from the pre-key message (type 0).
    std::fprintf(stderr, "[E2EE] DBG5: entering lock\n");
    std::string plaintext;
    // Recovery (HTTP) must never run while holding olmMtx_ — deferred here,
    // executed after the lock scope ends.
    std::string deferredRecovery;
    {
    std::lock_guard<std::mutex> lk(olmMtx_);
    std::fprintf(stderr, "[E2EE] DBG6: lock acquired\n");

    progressive::OlmSession session;
    auto* underlyingAccount = static_cast<progressive::OlmAccount*>(account_->rawAccount());
    std::fprintf(stderr, "[E2EE] DBG7: account ready\n");

    if (msgType == 0) {
        // Pre-key message — create inbound session, then decrypt
        // createInbound mutates the buffer via (void*) cast in olm.cpp:226.
        // Must pass a copy so the original body remains intact for decrypt.
        std::string msgCopy = body;
        std::fprintf(stderr, "[E2EE] DBG8: pre-key branch, calling createInbound bodySize=%zu\n", msgCopy.size());
        auto result = session.createInbound(*underlyingAccount, msgCopy);
        std::fprintf(stderr, "[E2EE] DBG9: createInbound success=%d\n", result.success ? 1 : 0);
        if (!result.success) {
            LOG(LogChannel::E2EE, "Olm: createInbound Olm session FAILED");
            auto* raw = static_cast<::OlmSession*>(session.rawSession());
            std::fprintf(stderr, "[E2EE] createInbound libolm error: %s\n",
                ::olm_session_last_error(raw) ? ::olm_session_last_error(raw) : "(null)");
            deferredRecovery = senderKey;
            goto olm_locked_end;
        }
        // After createInbound, decrypt the ORIGINAL message body
        std::fprintf(stderr, "[E2EE] DBG10: calling decrypt type 0\n");
        auto decResult = session.decrypt(body, 0);
        std::fprintf(stderr, "[E2EE] DBG11: decrypt success=%d dataSize=%zu\n",
            decResult.success ? 1 : 0, decResult.data.size());
        if (!decResult.success) {
            LOG(LogChannel::E2EE, "Olm: decrypt after createInbound FAILED");
            deferredRecovery = senderKey;
            goto olm_locked_end;
        }
        plaintext = decResult.data;
        std::fprintf(stderr, "[E2EE] DBG12: plaintext copied size=%zu\n", plaintext.size());

        std::fprintf(stderr, "[E2EE] DBG13: calling pickle\n");
        auto pickleResult = session.pickle("");
        std::fprintf(stderr, "[E2EE] DBG14: pickle success=%d size=%zu\n",
            pickleResult.success ? 1 : 0, pickleResult.data.size());
        if (pickleResult.success) {
            auto& vec = olmSessions_[senderKey];
            bool dup = false;
            for (const auto& existing : vec) {
                if (existing == pickleResult.data) { dup = true; break; }
            }
            if (!dup) {
                vec.push_back(pickleResult.data);
                if (vec.size() > 20) vec.erase(vec.begin());  // keep newest
            }
            LOG(LogChannel::E2EE, "Olm: saved session pickle for sender=%s (total=%zu)",
                senderKey.c_str(), vec.size());
        }
    } else {
        auto it = olmSessions_.find(senderKey);
        if (it == olmSessions_.end() || it->second.empty()) {
            LOG(LogChannel::E2EE, "Olm: no saved session for sender=%s — cannot decrypt type %d",
                senderKey.c_str(), msgType);
            deferredRecovery = senderKey;
            goto olm_locked_end;
        }
        bool decrypted = false;
        for (size_t i = 0; i < it->second.size(); ++i) {
            progressive::OlmSession sess;
            // libolm mutates the pickle buffer in-place via (void*) cast — pass a copy
            std::string pickleCopy = it->second[i];
            auto unpickleResult = sess.unpickle("", pickleCopy);
            if (!unpickleResult.success) {
                LOG(LogChannel::E2EE, "Olm: unpickle failed for sender=%s idx=%zu (keeping entry)",
                    senderKey.c_str(), i);
                continue;
            }
            auto decResult = sess.decrypt(body, 1);
            if (!decResult.success) {
                continue;
            }
            plaintext = decResult.data;
            auto rePickle = sess.pickle("");
            if (rePickle.success) {
                it->second[i] = rePickle.data;
            }
            decrypted = true;
            break;
        }
        if (!decrypted) {
            // The session chain with this device is broken (e.g. BAD_MESSAGE_MAC
            // after a restart lost the matching pickle). Drop the stale pickles
            // so the peer's next pre-key message starts clean, and send an
            // m.dummy pre-key so the peer re-establishes a session with us.
            LOG(LogChannel::E2EE, "Olm: decrypt type %d FAILED for sender=%s (tried %zu sessions) — re-establishing session",
                msgType, senderKey.c_str(), it->second.size());
            deferredRecovery = senderKey;
            goto olm_locked_end;
        }
    }
olm_locked_end:;
    }  // olmMtx_ released here — recovery does HTTP, never under the lock
    if (!deferredRecovery.empty()) {
        forceNewOlmSession(senderId, deferredRecovery);
        noteOlmRecovery(senderId, deferredRecovery);
        markOlmBroken(deferredRecovery);
    }

    // If we got plaintext, it's a JSON object like:
    //   {"type":"m.room_key","content":{...}}
    // If type == "m.room_key", call handleRoomKey.
    if (!plaintext.empty()) {
        simdjson::dom::parser pp;
        auto pd = pp.parse(plaintext);
        if (pd.error() == simdjson::SUCCESS) {
            auto t = pd.value()["type"].get_string();
            if (t.error() == simdjson::SUCCESS) {
                std::string_view typeVal(t.value());

                // Olm plaintext validation per m.olm.v1 spec
                auto senderVal = pd.value()["sender"].get_string();
                auto recipVal = pd.value()["recipient"].get_string();
                auto recipKeys = pd.value()["recipient_keys"]["ed25519"].get_string();
                auto senderKeys = pd.value()["keys"]["ed25519"].get_string();

                std::string ourUserId = ctxUserId_;
                std::string ourEd25519 = account_ ? account_->ed25519Key() : std::string();

                if (recipVal.error() == simdjson::SUCCESS && !ourUserId.empty() &&
                    std::string(recipVal.value()) != ourUserId) {
                    LOG(LogChannel::E2EE, "Olm: plaintext recipient mismatch — REJECTING");
                    return {};
                }
                if (recipKeys.error() == simdjson::SUCCESS && !ourEd25519.empty() &&
                    std::string(recipKeys.value()) != ourEd25519) {
                    LOG(LogChannel::E2EE, "Olm: plaintext recipient_keys mismatch — REJECTING");
                    return {};
                }
                if (senderVal.error() == simdjson::SUCCESS &&
                    std::string(senderVal.value()) != senderId) {
                    LOG(LogChannel::E2EE, "Olm: plaintext sender mismatch — REJECTING");
                    return {};
                }
                if (senderKeys.error() == simdjson::SUCCESS) {
                    LOG(LogChannel::E2EE, "Olm: sender keys.ed25519=%s (not yet verified — needs device key cache)",
                        std::string(senderKeys.value()).c_str());
                }

                auto cr = pd.value()["content"];
                std::string innerContent;
                if (cr.error() == simdjson::SUCCESS)
                    innerContent = simdjson::to_string(cr.value());
                if (typeVal == "m.room_key") {
                    LOG(LogChannel::E2EE, "Olm: inner type=m.room_key — calling handleRoomKey");
                    if (!innerContent.empty()) {
                        if (innerContent.find("\"sender_key\"") == std::string::npos) {
                            innerContent.insert(innerContent.size() - 1,
                                ",\"sender_key\":\"" + senderKey + "\"");
                        }
                        handleRoomKey(innerContent, senderId);
                    }
                } else if (typeVal == "m.room_key_request") {
                    // Another device's encrypted key request — route to the responder.
                    LOG(LogChannel::E2EE, "Olm: inner type=m.room_key_request — responding");
                    if (!innerContent.empty())
                        handleRoomKeyRequest(innerContent, senderId);
                } else if (typeVal == "m.forwarded_room_key") {
                    // An Olm-encrypted forwarded key — import + replay pending.
                    LOG(LogChannel::E2EE, "Olm: inner type=m.forwarded_room_key — importing");
                    if (!innerContent.empty())
                        handleForwardedRoomKey(innerContent, senderId);
                }
            }
        }
        return plaintext;
    }
    return {};
}

// ---- Outbound Megolm sessions ----

void Decryptor::setRoomEncryptionConfig(const std::string& roomId,
    const std::string& stateContentJson) {
    roomEncryptionConfigs_[roomId] = progressive::parseEncryptionConfig(stateContentJson);
}

std::string Decryptor::getOrCreateOutboundSession(const std::string& roomId) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    auto it = outboundSessions_.find(roomId);
    if (it != outboundSessions_.end()) {
        // Rotation: if the room's encryption config says this session is due
        // (message count or time period), drop it and create a fresh one.
        auto cfgIt = roomEncryptionConfigs_.find(roomId);
        if (cfgIt != roomEncryptionConfigs_.end() &&
            progressive::isRotationDue(cfgIt->second, it->second.messageCount,
                                       it->second.startTimeMs)) {
            LOG(LogChannel::E2EE, "getOrCreateOutboundSession: rotating session for room=%.40s",
                roomId.c_str());
            olm_clear_outbound_group_session(static_cast<::OlmOutboundGroupSession*>(
                it->second.session));
            free(it->second.session);
            outboundSessions_.erase(it);
        } else {
            return it->second.sessionId;
        }
    }

    // Create new outbound megolm session using libolm directly
    size_t sessionSize = olm_outbound_group_session_size();
    void* session = malloc(sessionSize);
    if (!session) return {};
    auto* olmSession = olm_outbound_group_session(session);
    size_t randLen = olm_init_outbound_group_session_random_length(olmSession);
    std::vector<uint8_t> random(randLen);
    fillCryptoRandom(random.data(), random.size());
    size_t ret = olm_init_outbound_group_session(olmSession, random.data(), random.size());
    if (ret == olm_error()) {
        free(session);
        return {};
    }

    // Get session ID
    size_t idLen = olm_outbound_group_session_id_length(olmSession);
    std::vector<uint8_t> idBuf(idLen);
    ret = olm_outbound_group_session_id(olmSession, idBuf.data(), idLen);
    if (ret == olm_error()) { free(session); return {}; }
    std::string sessionId(idBuf.begin(), idBuf.end());

    // Get session key (for sharing with other devices)
    size_t keyLen = olm_outbound_group_session_key_length(olmSession);
    std::vector<uint8_t> keyBuf(keyLen);
    ret = olm_outbound_group_session_key(olmSession, keyBuf.data(), keyLen);
    if (ret == olm_error()) { free(session); return {}; }
    std::string sessionKey(keyBuf.begin(), keyBuf.end());

    OutboundMegolmSession s;
    s.session = session;
    s.sessionId = sessionId;
    s.sessionKey = sessionKey;
    // Import outbound session as inbound so we can decrypt our own message echoes.
    megolm_->addInboundSession(roomId, curve25519Key(), sessionId, sessionKey);
    s.messageIndex = 0;
    s.messageCount = 0;
    s.startTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    outboundSessions_[roomId] = std::move(s);
    roomKeysShared_[roomId] = false;
    return sessionId;
}

std::string Decryptor::encryptMessage(const std::string& roomId,
                                        const std::string& deviceId,
                                        const std::string& plaintextEventJson) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    auto it = outboundSessions_.find(roomId);
    if (it == outboundSessions_.end()) {
        return {};  // no session — caller should call getOrCreateOutboundSession first
    }
    it->second.messageCount++;

    auto* olmSession = static_cast<::OlmOutboundGroupSession*>(it->second.session);
    // libolm overwrites the message buffer — copy plaintext
    size_t ciphertextLen = olm_group_encrypt_message_length(olmSession, plaintextEventJson.size());
    std::vector<uint8_t> ciphertext(ciphertextLen);
    size_t ret = olm_group_encrypt(olmSession,
        reinterpret_cast<uint8_t*>(const_cast<char*>(plaintextEventJson.data())),
        plaintextEventJson.size(),
        ciphertext.data(), ciphertextLen);
    if (ret == olm_error()) return {};

    // Build m.room.encrypted content
    std::string ciphertextB64(ciphertext.begin(), ciphertext.begin() + ret);
    auto senderKey = account_->curve25519Key();

    std::ostringstream out;
    out << "{\"algorithm\":\"m.megolm.v1.aes-sha2\""
        << ",\"ciphertext\":\"" << ciphertextB64 << "\""
        << ",\"sender_key\":\"" << senderKey << "\""
        << ",\"device_id\":\"" << deviceId << "\""
        << ",\"session_id\":\"" << it->second.sessionId << "\""
        << "}";
    return out.str();
}

std::string Decryptor::getOutboundSessionKey(const std::string& roomId) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    auto it = outboundSessions_.find(roomId);
    if (it == outboundSessions_.end()) return {};
    return it->second.sessionKey;
}

std::string Decryptor::getOutboundSessionId(const std::string& roomId) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    auto it = outboundSessions_.find(roomId);
    if (it == outboundSessions_.end()) return {};
    return it->second.sessionId;
}

bool Decryptor::hasOutboundSession(const std::string& roomId) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    return outboundSessions_.find(roomId) != outboundSessions_.end();
}

void Decryptor::dropOutboundSession(const std::string& roomId) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    auto it = outboundSessions_.find(roomId);
    if (it != outboundSessions_.end()) {
        if (it->second.session) {
            olm_clear_outbound_group_session(
                olm_outbound_group_session(it->second.session));
            free(it->second.session);
        }
        outboundSessions_.erase(it);
    }
    roomKeysShared_.erase(roomId);
}

bool Decryptor::roomKeyShared(const std::string& roomId) const {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    auto it = roomKeysShared_.find(roomId);
    return it != roomKeysShared_.end() && it->second;
}

void Decryptor::markRoomKeyShared(const std::string& roomId) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    roomKeysShared_[roomId] = true;
}

// ---- Room key sharing (full E2EE outbound) ----

// Helper: build auth headers for HTTP calls.
static std::unordered_map<std::string, std::string> makeAuthHeaders(const std::string& token) {
    return {{"Authorization", "Bearer " + token},
            {"Content-Type", "application/json"}};
}

// Helper: extract a string field from JSON (simdjson DOM).
static std::string domGetString(simdjson::dom::element parent, std::string_view key) {
    auto r = parent[key].get_string();
    if (r.error() == simdjson::SUCCESS) return std::string(r.value());
    return {};
}

bool Decryptor::shareRoomKey(const std::string& roomId,
                                const std::vector<std::string>& userIds,
                                const std::string& ourUserId,
                                const std::string& ourDeviceId,
                                const std::string& homeserverUrl,
                                const std::string& accessToken) {
    if (!isInitialized()) {
        std::fprintf(stderr, "[e2ee] shareRoomKey: not initialized\n");
        return false;
    }
    if (!hasOutboundSession(roomId)) {
        std::fprintf(stderr, "[e2ee] shareRoomKey: no outbound session for %s\n", roomId.c_str());
        return false;
    }

    auto ourCurve = curve25519Key();
    auto ourEd = ed25519Key();
    std::string sessionId = getOutboundSessionId(roomId);
    std::string sessionKey = getOutboundSessionKey(roomId);
    if (sessionKey.empty()) {
        std::fprintf(stderr, "[e2ee] shareRoomKey: empty session key\n");
        return false;
    }

    auto hdrs = makeAuthHeaders(accessToken);

    std::fprintf(stderr, "[e2ee] shareRoomKey: room=%.30s users=%zu\n",
                 roomId.c_str(), userIds.size());
    LOG(LogChannel::E2EE, "shareRoomKey: ourUserId=%s ourDeviceId=%s userIds=[%s]",
        ourUserId.c_str(), ourDeviceId.c_str(),
        [&]() { std::string s; for (size_t i=0; i<userIds.size(); ++i) {
            if (i) s += ","; s += userIds[i]; } return s; }().c_str());

    for (const auto& uid : userIds) {
        if (isDeviceStale(uid)) {
            LOG(LogChannel::E2EE, "shareRoomKey: user %s has stale device keys — querying fresh",
                uid.c_str());
            clearStale(uid);
        }
    }

    // Step 1: Query device keys for all room members — INCLUDING our own user
    // (our other devices need the room key too). The device loop below skips
    // only the sending device itself. Dedupe: a JSON object can't repeat keys.
    std::ostringstream queryBody;
    queryBody << "{\"device_keys\":{";
    bool first = true;
    for (size_t ui = 0; ui < userIds.size(); ++ui) {
        const auto& uid = userIds[ui];
        if (std::find(userIds.begin(), userIds.begin() + ui, uid) != userIds.begin() + ui)
            continue;  // keep the first occurrence of each user
        if (!first) queryBody << ",";
        first = false;
        queryBody << "\"" << uid << "\":[]";
    }
    queryBody << "}}";

    LOG(LogChannel::E2EE, "shareRoomKey: queryBody=%s", queryBody.str().c_str());

    auto queryResp = httpPost(homeserverUrl + "/_matrix/client/v3/keys/query",
                              queryBody.str(), hdrs, 30000);
    if (!queryResp.success) {
        LOG(LogChannel::E2EE, "shareRoomKey: keys/query FAILED http=%d bodyLen=%zu",
            queryResp.statusCode, queryResp.body.size());
        std::fprintf(stderr, "[e2ee] keys/query failed: %s\n", queryResp.errorMessage.c_str());
        return false;
    }

    LOG(LogChannel::E2EE, "shareRoomKey: keys/query ok http=%d bodyLen=%zu body=%.500s",
        queryResp.statusCode, queryResp.body.size(), queryResp.body.c_str());

    // Parse the response to extract device keys for each user.
    // Response format:
    //   {"device_keys":{"@user:server":{"device_id":{"algorithms":[...],
    //    "device_id":"...","keys":{"curve25519:dev":"...","ed25519:dev":"..."},
    //    "signatures":{...}}}},"failures":{}}
    simdjson::dom::parser parser;
    auto rootResult = parser.parse(queryResp.body);
    if (rootResult.error() != simdjson::SUCCESS) {
        std::fprintf(stderr, "[e2ee] keys/query response parse failed\n");
        return false;
    }

    struct DeviceInfo {
        std::string userId;
        std::string deviceId;
        std::string curve25519;
        std::string ed25519;
    };
    std::vector<DeviceInfo> devices;

    auto deviceKeysResult = rootResult.value()["device_keys"].get_object();
    if (deviceKeysResult.error() == simdjson::SUCCESS) {
        for (auto userField : deviceKeysResult.value()) {
            std::string uid(userField.key);
            size_t userDeviceCount = 0;
            auto userDevices = userField.value.get_object();
            if (userDevices.error() != simdjson::SUCCESS) continue;
            for (auto devField : userDevices.value()) {
                DeviceInfo info;
                info.userId = uid;
                info.deviceId = std::string(devField.key);
                if (info.deviceId == ourDeviceId && uid == ourUserId) continue;
                info.curve25519 = domGetString(devField.value, "curve25519:" + info.deviceId);
                // Extract curve25519 + ed25519 from keys object
                auto keysResult = devField.value["keys"].get_object();
                if (keysResult.error() == simdjson::SUCCESS) {
                    auto keysObj = keysResult.value();
                    for (auto k : keysObj) {
                        std::string kKey(k.key);
                        if (kKey.find("curve25519") != std::string::npos && info.curve25519.empty()) {
                            auto v = k.value.get_string();
                            if (v.error() == simdjson::SUCCESS) info.curve25519 = std::string(v.value());
                        }
                        if (kKey.find("ed25519") != std::string::npos) {
                            auto v = k.value.get_string();
                            if (v.error() == simdjson::SUCCESS) info.ed25519 = std::string(v.value());
                        }
                    }
                }
                if (!info.curve25519.empty() && !info.ed25519.empty()) {
                    auto sigResult = devField.value["signatures"][uid]["ed25519:" + info.deviceId].get_string();
                    std::string deviceSig = (sigResult.error() == simdjson::SUCCESS)
                        ? std::string(sigResult.value()) : "";
                    if (!deviceSig.empty()) {
                        if (!verifyDeviceKeys(uid, info.deviceId, info.curve25519, info.ed25519, deviceSig)) {
                            LOG(LogChannel::E2EE, "shareRoomKey: device key sig INVALID for %s/%s — SKIPPING",
                                uid.c_str(), info.deviceId.c_str());
                            continue;
                        }
                    }
                    devices.push_back(info);
                    userDeviceCount++;
                    std::fprintf(stderr, "[e2ee] found device: %s/%s curve=%s... (verified=%d)\n",
                                 uid.c_str(), info.deviceId.c_str(),
                                 info.curve25519.substr(0, 8).c_str(),
                                 !deviceSig.empty() ? 1 : 0);
                }
            }
            LOG(LogChannel::E2EE, "shareRoomKey: user=%s deviceCount=%zu",
                uid.c_str(), userDeviceCount);
        }
    }

    std::fprintf(stderr, "[e2ee] shareRoomKey: keys/query ok devices=%zu\n", devices.size());

    if (devices.empty()) {
        std::fprintf(stderr, "[e2ee] no devices to share room_key with\n");
        return false;
    }

    // Step 2: Claim one-time keys for each device. Group by user — a JSON
    // object can't repeat a key, so a device list like "@alice: A2, A1" must
    // become ONE entry with BOTH devices (else the parser keeps only the last
    // device and the other's OTK is never claimed — the multi-device CI test
    // caught this: shares with 2 devices of the same user failed for one).
    std::ostringstream claimBody;
    claimBody << "{\"one_time_keys\":{";
    bool firstUser = true;
    for (size_t di = 0; di < devices.size(); ++di) {
        if (di > 0 && devices[di].userId == devices[di - 1].userId) continue;
        if (!firstUser) claimBody << ",";
        firstUser = false;
        claimBody << "\"" << devices[di].userId << "\":{";
        bool firstDev = true;
        for (const auto& d : devices) {
            if (d.userId != devices[di].userId) continue;
            if (!firstDev) claimBody << ",";
            firstDev = false;
            claimBody << "\"" << d.deviceId << "\":\"signed_curve25519\"";
        }
        claimBody << "}";
    }
    claimBody << "}}";
    auto claimResp = httpPost(homeserverUrl + "/_matrix/client/v3/keys/claim",
                              claimBody.str(), hdrs, 15000);
    if (!claimResp.success) {
        std::fprintf(stderr, "[e2ee] keys/claim failed: %s\n", claimResp.errorMessage.c_str());
        return false;
    }

    // Parse the response to extract claimed one-time keys.
    // Response: {"one_time_keys":{"@user:server":{"device_id":
    //   {"signed_curve25519:AAAA":{"key":"...","signatures":{...}}}}}}
    simdjson::dom::parser claimParser;
    auto claimRoot = claimParser.parse(claimResp.body);
    if (claimRoot.error() != simdjson::SUCCESS) {
        std::fprintf(stderr, "[e2ee] keys/claim response parse failed\n");
        return false;
    }

    // For each device, find the claimed one-time key.
    struct ClaimedKey {
        std::string userId;
        std::string deviceId;
        std::string oneTimeKey;  // the actual key value
    };
    std::vector<ClaimedKey> claimedKeys;

    auto otkResult = claimRoot.value()["one_time_keys"].get_object();
    if (otkResult.error() == simdjson::SUCCESS) {
        for (auto userField : otkResult.value()) {
            std::string uid(userField.key);
            auto userDevs = userField.value.get_object();
            if (userDevs.error() != simdjson::SUCCESS) continue;
            for (auto devField : userDevs.value()) {
                std::string devId(devField.key);
                // The value is an object with one key: signed_curve25519:XXXX
                auto keyObj = devField.value.get_object();
                if (keyObj.error() != simdjson::SUCCESS) continue;
                for (auto k : keyObj.value()) {
                    ClaimedKey ck;
                    ck.userId = uid;
                    ck.deviceId = devId;
                    // The value has a "key" field
                    ck.oneTimeKey = domGetString(k.value, "key");
                    if (ck.oneTimeKey.empty()) {
                        // Maybe the value IS the key directly (some servers)
                        auto keyStr = k.value.get_string();
                        if (keyStr.error() == simdjson::SUCCESS) {
                            ck.oneTimeKey = std::string(keyStr.value());
                        }
                    }
                    if (!ck.oneTimeKey.empty()) {
                        auto otkSigResult = k.value["signatures"][uid]["ed25519:" + devId].get_string();
                        std::string otkSig = (otkSigResult.error() == simdjson::SUCCESS)
                            ? std::string(otkSigResult.value()) : "";
                        std::string devEd25519;
                        for (const auto& d : devices) {
                            if (d.userId == ck.userId && d.deviceId == ck.deviceId) {
                                devEd25519 = d.ed25519;
                                break;
                            }
                        }
                        if (!otkSig.empty() && !devEd25519.empty()) {
                            if (!verifyOtk(devEd25519, ck.oneTimeKey, otkSig)) {
                                // Retry the claim once — a fresh OTK may be
                                // served (stale keys are also consumed by
                                // claiming, so a retry often reaches a good one).
                                std::string retryBody = "{\"one_time_keys\":{\"" + ck.userId
                                    + "\":{\"" + ck.deviceId + "\":\"signed_curve25519\"}}}";
                                auto retryResp = httpPost(ctxHomeserver_
                                    + "/_matrix/client/v3/keys/claim", retryBody, hdrs, 15000);
                                bool retryOk = false;
                                if (retryResp.success) {
                                    simdjson::dom::parser rp;
                                    auto rdoc = rp.parse(retryResp.body);
                                    if (rdoc.error() == simdjson::SUCCESS) {
                                        auto rdev = rdoc.value()["one_time_keys"][ck.userId][ck.deviceId];
                                        auto rkeyObj = rdev.get_object();
                                        if (rkeyObj.error() == simdjson::SUCCESS) {
                                            for (auto rk : rkeyObj.value()) {
                                                if (std::string(rk.key).find("signed_curve25519:") != 0) continue;
                                                auto rkv = rk.value["key"].get_string();
                                                if (rkv.error() == simdjson::SUCCESS) {
                                                    auto rkSig = rk.value["signatures"][ck.userId]
                                                        ["ed25519:" + ck.deviceId].get_string();
                                                    if (rkSig.error() != simdjson::SUCCESS ||
                                                        verifyOtk(devEd25519,
                                                                  std::string(rkv.value()),
                                                                  std::string(rkSig.value()))) {
                                                        ck.oneTimeKey = std::string(rkv.value());
                                                        retryOk = true;
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (!retryOk) {
                                    LOG(LogChannel::E2EE,
                                        "shareRoomKey: OTK sig INVALID for %s/%s after re-claim — "
                                        "its OTK pool holds keys from an older identity",
                                        ck.userId.c_str(), ck.deviceId.c_str());
                                    break;
                                }
                            }
                        }
                        claimedKeys.push_back(ck);
                    }
                    break;  // only one key per device
                }
            }
        }
    }

    std::fprintf(stderr, "[e2ee] claimed %zu one-time keys (had %zu devices)\n",
                 claimedKeys.size(), devices.size());

    // Log the claimed key IDs (handy for diagnosing OTK-pool issues).
    std::string claimedIds;
    for (const auto& ck : claimedKeys) {
        if (!claimedIds.empty()) claimedIds += ",";
        claimedIds += ck.deviceId;
    }
    std::fprintf(stderr, "[e2ee] claimed device ids: %s\n", claimedIds.c_str());

    // Diagnose claim failures: which devices got keys and which were MISSING
    // (a device with no OTKs on the server comes back as an empty {} object).
    if (claimedKeys.size() != devices.size()) {
        LOG(LogChannel::E2EE, "shareRoomKey: claimResp body=%.800s", claimResp.body.c_str());
        for (const auto& d : devices) {
            bool got = false;
            for (const auto& ck : claimedKeys) {
                if (ck.userId == d.userId && ck.deviceId == d.deviceId) got = true;
            }
            std::fprintf(stderr, "[e2ee] claim result: %s/%s %s\n",
                         d.userId.c_str(), d.deviceId.c_str(), got ? "GOT" : "MISSING");
        }
    }

    if (claimedKeys.empty()) {
        std::fprintf(stderr, "[e2ee] no one-time keys claimed — can't share room_key\n");
        return false;
    }

    // Step 3: For each claimed key, create OlmSession outbound + encrypt m.room_key.
    // Build the /sendToDevice/m.room.encrypted body:
    //   {"messages":{"@user:server":{"device_id":{"algorithm":"m.olm.v1.curve25519-aes-sha2",
    //    "ciphertext":{"<their_curve>":{"body":"<base64>","type":0}},
    //    "sender_key":"<our_curve>"}}}}
    // Build the per-device messages grouped BY USER — a JSON object can't
    // repeat a key, so "@alice":{A1..},"@alice":{A2..} would drop one device
    // (server keeps the last). The multi-device CI test caught this: exactly
    // one of the sender's other devices randomly never got the room key.
    std::map<std::string, std::map<std::string, std::string>> perUserMsgs;
    int shared = 0;

    for (const auto& ck : claimedKeys) {
        // Find the matching device info for ed25519
        std::string theirEd;
        for (const auto& d : devices) {
            if (d.userId == ck.userId && d.deviceId == ck.deviceId) {
                theirEd = d.ed25519;
                break;
            }
        }
        std::string theirCurve;
        for (const auto& d : devices) {
            if (d.userId == ck.userId && d.deviceId == ck.deviceId) {
                theirCurve = d.curve25519;
                break;
            }
        }
        if (theirCurve.empty()) continue;
        if (theirEd.empty()) {
            std::fprintf(stderr, "[e2ee] shareRoomKey: no ed25519 for %s/%s — skipping\n",
                         ck.userId.c_str(), ck.deviceId.c_str());
            continue;
        }

        // Create OlmSession outbound
        progressive::OlmSession session;
        auto* underlyingAccount = static_cast<progressive::OlmAccount*>(account_->rawAccount());
        auto sessResult = session.createOutbound(*underlyingAccount, theirCurve, ck.oneTimeKey);
        if (!sessResult.success) {
            std::fprintf(stderr, "[e2ee] createOutbound failed for %s/%s\n",
                         ck.userId.c_str(), ck.deviceId.c_str());
            continue;
        }

        // Build the m.room_key plaintext JSON
        std::string roomKeyContent = "{\"algorithm\":\"m.megolm.v1.aes-sha2\","
            "\"room_id\":\"" + roomId + "\","
            "\"session_id\":\"" + sessionId + "\","
            "\"session_key\":\"" + sessionKey + "\"}";

        std::fprintf(stderr, "[e2ee] shareRoomKey: roomKeyContent=%s\n",
                     roomKeyContent.c_str());

        // Wrap it as a to-device event JSON:
        // {"type":"m.room_key","content":{...},"sender":"<our_user_id>","keys":{"ed25519":"<our_ed>"}}
        std::string plaintext = "{\"type\":\"m.room_key\",\"content\":" + roomKeyContent +
            ",\"sender\":\"" + ourUserId + "\""
            ",\"recipient\":\"" + ck.userId + "\""
            ",\"keys\":{\"ed25519\":\"" + ourEd + "\"}"
            ",\"recipient_keys\":{\"ed25519\":\"" + theirEd + "\"}}";

        // Encrypt with OlmSession
        auto encResult = session.encrypt(plaintext);
        if (!encResult.success || encResult.data.empty()) {
            std::fprintf(stderr, "[e2ee] Olm encrypt failed for %s/%s\n",
                         ck.userId.c_str(), ck.deviceId.c_str());
            continue;
        }

        std::fprintf(stderr, "[e2ee] shareRoomKey: encBody=%.200s\n",
                     encResult.data.c_str());

        // Build the per-device ciphertext entry
        std::string deviceMsg = "{\"algorithm\":\"m.olm.v1.curve25519-aes-sha2\","
            "\"ciphertext\":{\"" + theirCurve + "\":{"
            "\"body\":\"" + encResult.data + "\","
            "\"type\":0}},"
            "\"sender_key\":\"" + ourCurve + "\"}";
        perUserMsgs[ck.userId][ck.deviceId] = std::move(deviceMsg);
        shared++;
    }

    std::ostringstream sendBody;
    sendBody << "{\"messages\":{";
    bool firstUserMsg = true;
    for (const auto& [uid, devs] : perUserMsgs) {
        if (!firstUserMsg) sendBody << ",";
        firstUserMsg = false;
        sendBody << "\"" << uid << "\":{";
        bool firstDev = true;
        for (const auto& [devId, msg] : devs) {
            if (!firstDev) sendBody << ",";
            firstDev = false;
            sendBody << "\"" << devId << "\":" << msg;
        }
        sendBody << "}";
    }
    sendBody << "}}";

    std::fprintf(stderr, "[e2ee] shareRoomKey: sendBody=%.600s\n",
                 sendBody.str().c_str());

    if (shared == 0) {
        std::fprintf(stderr, "[e2ee] failed to encrypt room_key for any device\n");
        return false;
    }

    // Step 4: Send m.room.encrypted to-device event.
    std::string txnId = "pdkey" + std::to_string(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
        + g_txnCounter.fetch_add(1));
    std::string url = homeserverUrl + "/_matrix/client/v3/sendToDevice/m.room.encrypted/" + txnId;
    auto sendResp = httpPut(url, sendBody.str(), hdrs, 15000);
    if (!sendResp.success) {
        std::fprintf(stderr, "[e2ee] sendToDevice failed: %s\n", sendResp.errorMessage.c_str());
        return false;
    }

    std::fprintf(stderr, "[e2ee] shareRoomKey: sendToDevice ok shared=%d\n", shared);

    std::fprintf(stderr, "[e2ee] shared room_key with %d device(s) for room %s\n",
                 shared, roomId.c_str());
    return true;
}

size_t Decryptor::olmSessionCount() {
    std::lock_guard<std::mutex> lk(olmMtx_);
    size_t total = 0;
    for (const auto& [k, v] : olmSessions_) total += v.size();
    return total;
}

void Decryptor::setCryptoContext(const std::string& ourUserId, const std::string& ourDeviceId,
                                  const std::string& homeserverUrl, const std::string& accessToken) {
    // The outbound-session cache is tied to our identity keys — a re-init
    // (e.g. after a device reset) regenerates them, so stale sessions must go.
    {
        std::lock_guard<std::mutex> lk(outboundOlmMtx_);
        outboundOlmSessions_.clear();
    }
    ctxUserId_ = ourUserId;
    ctxDeviceId_ = ourDeviceId;
    ctxHomeserver_ = homeserverUrl;
    ctxToken_ = accessToken;
}

// Send a key request to a device list; "*" entries go as PLAIN to-device
// (Olm encryption to a wildcard is impossible).
bool Decryptor::sendKeyRequestToDevices(const std::string& senderId,
    const std::vector<std::string>& devices, const std::string& requestContent) {
    bool ok = false;
    std::string plainBody;
    for (const auto& dev : devices) {
        if (dev == "*") {
            plainBody = "{\"messages\":{\"" + senderId + "\":{\"*\":"
                + requestContent + "}}}";
            continue;
        }
        ok |= sendOlmToDevice(senderId, dev, "m.room_key_request", requestContent, true);
    }
    if (!plainBody.empty()) {
        std::string txn = "pdplain" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        auto resp = httpPut(ctxHomeserver_ + "/_matrix/client/v3/sendToDevice/m.room_key_request/"
            + txn, plainBody, makeAuthHeaders(ctxToken_), 15000);
        LOG(LogChannel::E2EE, "sendKeyRequestToDevices: plain wildcard send ok=%d status=%d",
            resp.success ? 1 : 0, resp.statusCode);
        ok = ok || resp.success;
    }
    return ok;
}

// Shared builder for the m.room_key_request content (used by the initial
// request, the backoff retries, and the manual "Ask for keys" resend).
static std::string buildRoomKeyRequestJson(const std::string& roomId,
    const std::string& senderKey, const std::string& sessionId,
    const std::string& requestId, const std::string& requestingDeviceId) {
    return "{\"action\":\"request\","
        "\"body\":{\"algorithm\":\"m.megolm.v1.aes-sha2\","
        "\"room_id\":\"" + roomId + "\","
        "\"sender_key\":\"" + senderKey + "\","
        "\"session_id\":\"" + sessionId + "\"},"
        "\"request_id\":\"" + requestId + "\","
        "\"requesting_device_id\":\"" + requestingDeviceId + "\"}";
}

void Decryptor::requestRoomKey(const std::string& roomId, const std::string& senderId,
                                const std::string& senderKey, const std::string& sessionId,
                                const std::string& senderDeviceId) {
    if (ctxHomeserver_.empty() || ctxToken_.empty() || senderId.empty()) return;
    std::string key = roomId + "|" + sessionId + "|" + senderKey;
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        if (requestedKeys_.count(key)) return;  // retries go through maybeReRequestKeys
        KeyRequestState st;
        st.attempts = 1;  // this first request counts
        st.lastMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        st.senderId = senderId;
        st.senderDeviceId = senderDeviceId;
        requestedKeys_.emplace(key, std::move(st));
        // History loads accumulate unresolved sessions — cap the map and
        // evict the oldest (matching recentKeyRequests_' pattern).
        if (requestedKeys_.size() > 200) {
            auto oldest = requestedKeys_.begin();
            for (auto it2 = requestedKeys_.begin(); it2 != requestedKeys_.end(); ++it2)
                if (it2->second.lastMs < oldest->second.lastMs) oldest = it2;
            requestedKeys_.erase(oldest);
        }
    }
    std::string reqId = "pdrkr" + std::to_string(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
        + g_txnCounter.fetch_add(1));
    // The request content (m.room_key_request) — sent Olm-encrypted per spec.
    std::string requestContent = buildRoomKeyRequestJson(roomId, senderKey, sessionId, reqId, ctxDeviceId_);
    // Ask ALL of the sender's devices (Element parity) — one request per
    // device, Olm-encrypted. Falls back to the known sender device, then to
    // the "*" wildcard so a request is never silently dropped.
    auto devs = resolveRequestRecipients(senderId, senderDeviceId);
    // History loads can hit many missing sessions at once — cap the actual
    // sends; deferred entries stay in the map and are retried on schedule.
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - lastRequestGateMs_ > 5000) { requestGateCount_ = 0; lastRequestGateMs_ = nowMs; }
        if (requestGateCount_ >= 10) {
            auto it = requestedKeys_.find(key);
            if (it != requestedKeys_.end()) it->second.lastMs = nowMs - 25000;
            LOG(LogChannel::E2EE, "requestRoomKey: rate-limited room=%.40s sid=%.20s (deferred)",
                roomId.c_str(), sessionId.c_str());
            return;
        }
        requestGateCount_++;
    }
    bool ok = sendKeyRequestToDevices(senderId, devs, requestContent);
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        auto it = requestedKeys_.find(key);
        if (it != requestedKeys_.end()) {
            it->second.lastRequestId = reqId;
            it->second.recipientDevices = devs;
        }
    }
    LOG(LogChannel::E2EE, "requestRoomKey: sent for room=%.40s sid=%.20s sender=%s ok=%d",
        roomId.c_str(), sessionId.c_str(), senderId.c_str(), ok ? 1 : 0);
    std::fprintf(stderr, "[e2ee] requestRoomKey: room=%.40s sid=%.20s sender=%s ok=%d\n",
                 roomId.c_str(), sessionId.c_str(), senderId.c_str(), ok ? 1 : 0);
    noteRoomKey({roomId, sessionId, senderId, RoomKeyEventKind::Requested, 1, 0});
}

void Decryptor::reRequestKey(const std::string& roomId, const std::string& senderId,
                             const std::string& senderKey, const std::string& sessionId,
                             const std::string& senderDeviceId) {
    if (ctxHomeserver_.empty() || ctxToken_.empty() || senderId.empty()) return;
    std::string key = roomId + "|" + sessionId + "|" + senderKey;
    int attempt = 1;
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        auto it = requestedKeys_.find(key);
        if (it != requestedKeys_.end()) {
            it->second.lastMs = nowMs;
            it->second.senderId = senderId;
            it->second.senderDeviceId = senderDeviceId;
            attempt = it->second.attempts;
        } else {
            KeyRequestState st;
            st.attempts = 1;
            st.lastMs = nowMs;
            st.senderId = senderId;
            st.senderDeviceId = senderDeviceId;
            requestedKeys_.emplace(key, std::move(st));
        }
    }
    std::string reqId = "pdrkr" + std::to_string(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
        + g_txnCounter.fetch_add(1));
    std::string requestContent = buildRoomKeyRequestJson(roomId, senderKey, sessionId, reqId, ctxDeviceId_);
    auto devs = resolveRequestRecipients(senderId, senderDeviceId);
    bool ok = sendKeyRequestToDevices(senderId, devs, requestContent);
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        auto it = requestedKeys_.find(key);
        if (it != requestedKeys_.end()) {
            it->second.lastRequestId = reqId;
            it->second.recipientDevices = devs;
        }
    }
    noteRoomKey({roomId, sessionId, senderId, RoomKeyEventKind::Requested, attempt, 0});
    LOG(LogChannel::E2EE, "reRequestKey: manual room=%.40s sid=%.20s sender=%s ok=%d",
        roomId.c_str(), sessionId.c_str(), senderId.c_str(), ok ? 1 : 0);
}

// Element parity: after SAS verification, re-ask every outstanding key.
void Decryptor::resendAllPendingRequests() {
    if (ctxHomeserver_.empty() || ctxToken_.empty()) return;
    std::vector<std::pair<std::string, KeyRequestState>> entries;
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        for (const auto& [key, st] : requestedKeys_) entries.emplace_back(key, st);
    }
    for (auto& [key, st] : entries) {
        auto sep1 = key.find('|');
        auto sep2 = key.find('|', sep1 == std::string::npos ? 0 : sep1 + 1);
        if (sep1 == std::string::npos || sep2 == std::string::npos) continue;
        std::string roomId = key.substr(0, sep1);
        std::string sessionId = key.substr(sep1 + 1, sep2 - sep1 - 1);
        std::string senderKey = key.substr(sep2 + 1);
        std::string reqId = "pdrkr" + std::to_string(
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
            + g_txnCounter.fetch_add(1));
        std::string requestContent = buildRoomKeyRequestJson(roomId, senderKey, sessionId, reqId, ctxDeviceId_);
        std::vector<std::string> devs = st.recipientDevices;
        if (devs.empty() && !st.senderDeviceId.empty()) devs.push_back(st.senderDeviceId);
        if (devs.empty()) devs.push_back("*");
        bool ok = sendKeyRequestToDevices(st.senderId, devs, requestContent);
        {
            std::lock_guard<std::mutex> lk(requestMtx_);
            auto it = requestedKeys_.find(key);
            if (it != requestedKeys_.end()) {
                it->second.attempts++;
                it->second.lastMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                it->second.lastRequestId = reqId;
                it->second.recipientDevices = devs;
            }
        }
        noteRoomKey({roomId, sessionId, st.senderId, RoomKeyEventKind::Requested, st.attempts + 1, 0});
        LOG(LogChannel::E2EE, "resendAllPendingRequests: room=%.40s sid=%.20s ok=%d",
            roomId.c_str(), sessionId.c_str(), ok ? 1 : 0);
    }
}

bool Decryptor::resetIdentity() {
    if (!account_) return false;
    clearPerAccountState();
    if (!account_->reset()) return false;
    if (!account_->create()) return false;
    LOG(LogChannel::E2EE, "resetIdentity: new identity keys generated — 1:1 sessions cleared");
    return true;
}

// Persist outstanding key requests (survive restarts).
std::string Decryptor::picklePendingKeyRequests() {
    std::lock_guard<std::mutex> lk(requestMtx_);
    if (requestedKeys_.empty()) return "[]";
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& [key, st] : requestedKeys_) {
        auto sep1 = key.find('|');
        auto sep2 = key.find('|', sep1 == std::string::npos ? 0 : sep1 + 1);
        if (sep1 == std::string::npos || sep2 == std::string::npos) continue;
        if (!first) os << ",";
        first = false;
        os << "{\"r\":\"" << key.substr(0, sep1)
           << "\",\"s\":\"" << key.substr(sep1 + 1, sep2 - sep1 - 1)
           << "\",\"k\":\"" << key.substr(sep2 + 1)
           << "\",\"u\":\"" << st.senderId
           << "\",\"d\":\"" << st.senderDeviceId
           << "\",\"a\":" << st.attempts
           << ",\"l\":" << st.lastMs
           << ",\"q\":\"" << st.lastRequestId << "\"}";
    }
    os << "]";
    return os.str();
}

bool Decryptor::unpicklePendingKeyRequests(const std::string& json) {
    if (json.empty() || json == "[]") return true;
    simdjson::dom::parser p;
    auto doc = p.parse(json);
    if (doc.error() != simdjson::SUCCESS) return false;
    auto arr = doc.value().get_array();
    if (arr.error() != simdjson::SUCCESS) return false;
    std::lock_guard<std::mutex> lk(requestMtx_);
    int loaded = 0;
    for (auto elem : arr.value()) {
        auto obj = elem.get_object();
        if (obj.error() != simdjson::SUCCESS) continue;
        auto r = obj.value()["r"].get_string();
        auto s = obj.value()["s"].get_string();
        auto k = obj.value()["k"].get_string();
        auto u = obj.value()["u"].get_string();
        if (r.error() != simdjson::SUCCESS || s.error() != simdjson::SUCCESS ||
            k.error() != simdjson::SUCCESS || u.error() != simdjson::SUCCESS) continue;
        std::string key = std::string(r.value()) + "|" + std::string(s.value()) + "|" + std::string(k.value());
        if (requestedKeys_.count(key)) continue;
        KeyRequestState st;
        st.senderId = std::string(u.value());
        auto d = obj.value()["d"].get_string();
        if (d.error() == simdjson::SUCCESS) st.senderDeviceId = std::string(d.value());
        auto q = obj.value()["q"].get_string();
        if (q.error() == simdjson::SUCCESS) st.lastRequestId = std::string(q.value());
        auto a = obj.value()["a"].get_int64();
        if (a.error() == simdjson::SUCCESS) st.attempts = static_cast<int>(a.value());
        auto l = obj.value()["l"].get_int64();
        if (l.error() == simdjson::SUCCESS) st.lastMs = l.value();
        requestedKeys_.emplace(key, std::move(st));
        ++loaded;
    }
    LOG(LogChannel::E2EE, "unpicklePendingKeyRequests: loaded %d", loaded);
    return true;
}

// Resolve the devices to ask: query the sender's device list, fall back to
// the known sender device, then to the "*" wildcard.
std::vector<std::string> Decryptor::resolveRequestRecipients(
    const std::string& senderId, const std::string& senderDeviceId) {
    std::vector<std::string> devs;
    if (!senderId.empty() && !ctxHomeserver_.empty() && !ctxToken_.empty()) {
        std::string queryBody = "{\"device_keys\":{\"" + senderId + "\":[]}}";
        auto resp = httpPost(ctxHomeserver_ + "/_matrix/client/v3/keys/query",
                             queryBody, makeAuthHeaders(ctxToken_), 10000);
        if (resp.success && !resp.body.empty()) {
            simdjson::dom::parser p;
            auto doc = p.parse(resp.body);
            if (doc.error() == simdjson::SUCCESS) {
                auto userObj = doc.value()["device_keys"][senderId];
                if (userObj.error() == simdjson::SUCCESS) {
                    auto obj = userObj.value().get_object();
                    if (obj.error() == simdjson::SUCCESS) {
                        for (auto field : obj.value())
                            devs.emplace_back(field.key);
                    }
                }
            }
        }
    }
    if (devs.empty() && !senderDeviceId.empty()) devs.push_back(senderDeviceId);
    if (devs.empty()) devs.push_back("*");
    return devs;
}

// Tell the sender its request is no longer needed (Element parity).
void Decryptor::sendRequestCancellation(const KeyRequestState& st) {
    if (st.lastRequestId.empty() || st.senderId.empty()) return;
    std::string content = "{\"action\":\"request_cancellation\","
        "\"requesting_device_id\":\"" + ctxDeviceId_ + "\","
        "\"request_id\":\"" + st.lastRequestId + "\"}";
    std::vector<std::string> devs = st.recipientDevices;
    if (devs.empty() && !st.senderDeviceId.empty()) devs.push_back(st.senderDeviceId);
    if (devs.empty()) devs.push_back("*");
    for (const auto& dev : devs)
        sendOlmToDevice(st.senderId, dev, "m.room_key_request", content);
    LOG(LogChannel::E2EE, "sendRequestCancellation: sender=%s reqId=%.30s",
        st.senderId.c_str(), st.lastRequestId.c_str());
}


void Decryptor::maybeReRequestKeys() {
    if (ctxHomeserver_.empty() || ctxToken_.empty()) return;
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(requestMtx_);
    for (auto& [key, st] : requestedKeys_) {
        int64_t elapsed = nowMs - st.lastMs;
        if (!shouldReRequestKey(st.attempts, elapsed)) {
            // After the final attempt the backoff stops forever — surface
            // the dead end once so the room doesn't just say "waiting".
            if (st.attempts > 4 && !st.gaveUpNotified) {
                st.gaveUpNotified = true;
                auto sep1 = key.find('|');
                auto sep2 = key.find('|', sep1 == std::string::npos ? 0 : sep1 + 1);
                if (sep1 != std::string::npos && sep2 != std::string::npos) {
                    noteRoomKey({key.substr(0, sep1),
                                 key.substr(sep1 + 1, sep2 - sep1 - 1),
                                 st.senderId, RoomKeyEventKind::GaveUp, 0, 0});
                }
            }
            continue;
        }
        st.attempts++;
        st.lastMs = nowMs;
        std::string reqId = "pdrkr" + std::to_string(
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
            + g_txnCounter.fetch_add(1));
        auto sep1 = key.find('|');
        auto sep2 = key.find('|', sep1 == std::string::npos ? 0 : sep1 + 1);
        if (sep1 == std::string::npos || sep2 == std::string::npos) continue;
        std::string roomId = key.substr(0, sep1);
        std::string sessionId = key.substr(sep1 + 1, sep2 - sep1 - 1);
        std::string senderKey = key.substr(sep2 + 1);
        std::string requestContent = buildRoomKeyRequestJson(roomId, senderKey, sessionId, reqId, ctxDeviceId_);
        std::vector<std::string> devs = st.recipientDevices;
        if (devs.empty() && !st.senderDeviceId.empty()) devs.push_back(st.senderDeviceId);
        if (devs.empty()) devs.push_back("*");
        bool ok = sendKeyRequestToDevices(st.senderId, devs, requestContent);
        st.lastRequestId = reqId;
        noteRoomKey({roomId, sessionId, st.senderId, RoomKeyEventKind::Requested, st.attempts, 0});
        LOG(LogChannel::E2EE, "maybeReRequestKeys: retry %d for room=%.40s sid=%.20s ok=%d",
            st.attempts, roomId.c_str(), sessionId.c_str(), ok ? 1 : 0);
    }
}

bool Decryptor::handleRoomKeyRequest(const std::string& contentJson,
    const std::string& senderId) {
    if (ctxHomeserver_.empty() || ctxToken_.empty()) return false;

    simdjson::dom::parser p;
    auto doc = p.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) return false;
    auto val = doc.value();

    auto action = val["action"].get_string();
    if (action.error() != simdjson::SUCCESS || std::string(action.value()) != "request")
        return false;

    auto body = val["body"].get_object();
    if (body.error() != simdjson::SUCCESS) return false;

    auto alg = body.value()["algorithm"].get_string();
    if (alg.error() != simdjson::SUCCESS || std::string(alg.value()) != "m.megolm.v1.aes-sha2")
        return false;
    auto rid = body.value()["room_id"].get_string();
    auto sid = body.value()["session_id"].get_string();
    auto skey = body.value()["sender_key"].get_string();
    auto reqDev = val["requesting_device_id"].get_string();
    if (reqDev.error() != simdjson::SUCCESS)
        reqDev = body.value()["requesting_device_id"].get_string();
    auto reqId = val["request_id"].get_string();
    if (reqId.error() != simdjson::SUCCESS)
        reqId = body.value()["request_id"].get_string();
    if (rid.error() != simdjson::SUCCESS || sid.error() != simdjson::SUCCESS ||
        skey.error() != simdjson::SUCCESS || reqId.error() != simdjson::SUCCESS ||
        reqDev.error() != simdjson::SUCCESS)
        return false;

    std::string roomId(rid.value()), sessionId(sid.value());
    std::string senderKey(skey.value()), requestId(reqId.value());
    std::string requestingDeviceId(reqDev.value());

    // Dedup by request_id (re-share once per request).
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        if (recentKeyRequests_.count(requestId)) return false;
        if (recentKeyRequests_.size() >= 200) recentKeyRequests_.clear();
        recentKeyRequests_.insert(requestId);
    }

    // Policy: verified-only mode requires the requesting device to be SAS-verified.
    if (shareKeysVerifiedOnly_) {
        bool verified = false;
        if (verifiedDeviceChecker_) {
            simdjson::dom::parser vp;
            auto vd = vp.parse(contentJson);
            if (vd.error() == simdjson::SUCCESS) {
                auto rd = vd.value()["requesting_device_id"].get_string();
                if (rd.error() != simdjson::SUCCESS) {
                    auto b = vd.value()["body"].get_object();
                    if (b.error() == simdjson::SUCCESS)
                        rd = b.value()["requesting_device_id"].get_string();
                }
                if (rd.error() == simdjson::SUCCESS)
                    verified = verifiedDeviceChecker_(senderId, std::string(rd.value()));
            }
        }
        if (!verified) return false;
    }
    // We must actually hold the requested session.
    if (!megolm_->hasSession(roomId, senderKey, sessionId)) return false;

    // Export the session key (v1 export format) and build m.forwarded_room_key.
    std::string sessionKey = megolm_->exportSessionKey(roomId, senderKey, sessionId);
    if (sessionKey.empty()) return false;

    std::ostringstream content;
    content << "{\"algorithm\":\"m.megolm.v1.aes-sha2\","
            << "\"room_id\":\"" << roomId << "\","
            << "\"session_id\":\"" << sessionId << "\","
            << "\"session_key\":\"" << sessionKey << "\","
            << "\"sender_key\":\"" << senderKey << "\","
            << "\"forwarding_curve25519_key_chain\":[],"
            << "\"org.matrix.msc3061.shared_history\":true}";

    bool ok = sendOlmToDevice(senderId, requestingDeviceId,
        "m.forwarded_room_key", content.str());
    LOG(LogChannel::E2EE, "handleRoomKeyRequest: forwarded key room=%.40s sid=%.20s to=%s/%s ok=%d",
        roomId.c_str(), sessionId.c_str(), senderId.c_str(),
        requestingDeviceId.c_str(), ok ? 1 : 0);
    return ok;
}



// Handle an incoming m.forwarded_room_key — import the v1 export-format
// session key so we can decrypt messages the other device forwarded.
bool Decryptor::handleForwardedRoomKey(const std::string& contentJson,
    const std::string& senderId) {
    simdjson::dom::parser p;
    auto doc = p.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) return false;
    auto val = doc.value();

    auto alg = val["algorithm"].get_string();
    if (alg.error() != simdjson::SUCCESS || std::string(alg.value()) != "m.megolm.v1.aes-sha2")
        return false;
    auto rid = val["room_id"].get_string();
    auto skey = val["sender_key"].get_string();
    auto sessKey = val["session_key"].get_string();
    if (rid.error() != simdjson::SUCCESS || skey.error() != simdjson::SUCCESS ||
        sessKey.error() != simdjson::SUCCESS)
        return false;

    std::string roomId(rid.value());
    std::string senderKey(skey.value());
    std::string sessionKeyExport(sessKey.value());

    std::string realId = megolm_->addImportedSession(roomId, senderKey, sessionKeyExport);
    if (realId.empty()) {
        LOG(LogChannel::E2EE, "handleForwardedRoomKey: import FAILED room=%.40s sender=%.20s",
            roomId.c_str(), senderKey.c_str());
        return false;
    }
    // Replay any events that were pending on this session (they triggered the
    // key request this forwarded key is answering).
    processPending(roomId, senderKey, realId);
    noteRoomKey({roomId, realId, senderId, RoomKeyEventKind::Received, 0, 0});
    // A forwarded key satisfies any pending requests for this room+session
    // (the imported session id can differ from the requested one). Without
    // this the backoff retries would keep firing forever.
    {
        std::vector<KeyRequestState> satisfied;
        {
            std::lock_guard<std::mutex> lk(requestMtx_);
            for (auto it = requestedKeys_.begin(); it != requestedKeys_.end();) {
                auto sep1 = it->first.find('|');
                auto sep2 = it->first.find('|', sep1 == std::string::npos ? 0 : sep1 + 1);
                bool match = sep1 != std::string::npos && sep2 != std::string::npos &&
                             it->first.compare(0, sep1, roomId) == 0 &&
                             it->first.compare(sep2 + 1, std::string::npos, senderKey) == 0;
                if (match) {
                    LOG(LogChannel::E2EE, "handleForwardedRoomKey: satisfied pending request for room=%.40s sid=%.20s",
                        roomId.c_str(), it->first.substr(sep1 + 1, sep2 - sep1 - 1).c_str());
                    satisfied.push_back(it->second);
                    it = requestedKeys_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& st : satisfied) sendRequestCancellation(st);
    }
    LOG(LogChannel::E2EE, "handleForwardedRoomKey: imported room=%.40s sender=%.20s",
        roomId.c_str(), senderKey.c_str());
    return true;
}



// Export all megolm keys (inbound + outbound) as a MegolmSessionData envelope.
// Export all megolm keys (inbound + self-echo) as a MegolmSessionData
// envelope. The outbound sessions are intentionally NOT exported: the initial
// outbound key cannot decrypt past the ratchet and cannot resume sending —
// the self-echo inbound session (added at outbound creation) covers every
// sender-room with a usable export at the correct firstKnownIndex.
std::string Decryptor::exportAllKeys() {
    return megolm_->exportAllJson();
}

// Import a MegolmSessionData envelope (v1). Returns count imported.
int Decryptor::importKeys(const std::string& json) {
    simdjson::dom::parser p;
    auto doc = p.parse(json);
    if (doc.error() != simdjson::SUCCESS) return 0;
    auto rooms = doc.value()["rooms"].get_object();
    if (rooms.error() != simdjson::SUCCESS) return 0;
    int imported = 0;
    for (auto room : rooms.value()) {
        std::string roomId(room.key);
        auto sessions = room.value["sessions"].get_array();
        if (sessions.error() != simdjson::SUCCESS) continue;
        for (auto sess : sessions.value()) {
            auto skey = sess["sender_key"].get_string();
            auto key = sess["session_key"].get_string();
            if (skey.error() != simdjson::SUCCESS || key.error() != simdjson::SUCCESS) continue;
            std::string realId = megolm_->addImportedSession(
                roomId, std::string(skey.value()), std::string(key.value()));
            if (!realId.empty()) {
                imported++;
                processPending(roomId, std::string(skey.value()), realId);
            }
        }
    }
    LOG(LogChannel::E2EE, "importKeys: imported %d sessions", imported);
    return imported;
}



std::string Decryptor::importSingleSession(const std::string& roomId,
                                           const std::string& senderKey,
                                           const std::string& exportBase64) {
    std::string realId = megolm_->addImportedSession(roomId, senderKey, exportBase64);
    if (!realId.empty()) processPending(roomId, senderKey, realId);
    return realId;
}

// Send an Olm-encrypted to-device event to a single device: query keys ->
// claim OTK -> create outbound session -> encrypt inner event -> PUT
// /sendToDevice/m.room.encrypted. Matches shareRoomKey's envelope format.
bool Decryptor::sendOlmToDevice(const std::string& targetUserId,
    const std::string& targetDeviceId, const std::string& innerType,
    const std::string& innerContent, bool forceFresh) {
    if (ctxHomeserver_.empty() || ctxToken_.empty()) return false;
    auto hdrs = makeAuthHeaders(ctxToken_);

    // 1. Query the target device's keys (curve25519 + ed25519).
    std::string queryBody = "{\"device_keys\":{\"" + targetUserId + "\":[]}}";
    auto queryResp = httpPost(ctxHomeserver_ + "/_matrix/client/v3/keys/query",
                              queryBody, hdrs, 15000);
    if (!queryResp.success) return false;
    std::string theirCurve, theirEd, deviceSig;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(queryResp.body);
        if (doc.error() != simdjson::SUCCESS) return false;
        auto devObj = doc.value()["device_keys"][targetUserId][targetDeviceId];
        auto keysObj = devObj["keys"].get_object();
        if (keysObj.error() != simdjson::SUCCESS) return false;
        for (auto k : keysObj.value()) {
            auto v = k.value.get_string();
            if (v.error() != simdjson::SUCCESS) continue;
            std::string key(k.key);
            if (key == "curve25519:" + targetDeviceId) theirCurve = std::string(v.value());
            else if (key == "ed25519:" + targetDeviceId) theirEd = std::string(v.value());
        }
        auto sig = devObj["signatures"][targetUserId]["ed25519:" + targetDeviceId].get_string();
        if (sig.error() == simdjson::SUCCESS) deviceSig = std::string(sig.value());
    }
    if (theirCurve.empty() || theirEd.empty()) return false;
    // Verify the device key signature (mirror shareRoomKey) — fail closed.
    if (!deviceSig.empty() &&
        !verifyDeviceKeys(targetUserId, targetDeviceId, theirCurve, theirEd, deviceSig)) {
        LOG(LogChannel::E2EE, "sendOlmToDevice: device key sig INVALID for %s/%s — refusing",
            targetUserId.c_str(), targetDeviceId.c_str());
        return false;
    }

    // 2. Claim an OTK (fallback key returned when the pool is exhausted).
    // Retry once with a fresh claim on signature failure: the server may hand
    // out a different (fresh) OTK — claiming also consumes stale keys, so a
    // retry often reaches a valid one.
    std::string oneTimeKey, otkSig;
    auto claimAndVerify = [&](bool secondAttempt) {
        std::string claimBody = "{\"one_time_keys\":{\"" + targetUserId
            + "\":{\"" + targetDeviceId + "\":\"signed_curve25519\"}}}";
        auto claimResp = httpPost(ctxHomeserver_ + "/_matrix/client/v3/keys/claim",
                                  claimBody, hdrs, 15000);
        if (!claimResp.success) return false;
        oneTimeKey.clear();
        otkSig.clear();
        {
            simdjson::dom::parser p;
            auto doc = p.parse(claimResp.body);
            if (doc.error() != simdjson::SUCCESS) return false;
            auto devObj = doc.value()["one_time_keys"][targetUserId][targetDeviceId];
            auto keyObj = devObj.get_object();
            if (keyObj.error() != simdjson::SUCCESS) return false;
            for (auto k : keyObj.value()) {
                if (std::string(k.key).find("signed_curve25519:") != 0) continue;
                auto kv = k.value["key"].get_string();
                if (kv.error() == simdjson::SUCCESS) oneTimeKey = std::string(kv.value());
                auto sig = k.value["signatures"][targetUserId]["ed25519:" + targetDeviceId].get_string();
                if (sig.error() == simdjson::SUCCESS) otkSig = std::string(sig.value());
                break;
            }
        }
        if (oneTimeKey.empty()) return false;
        if (otkSig.empty()) return true;  // nothing to verify (server quirk)
        return verifyOtk(theirEd, oneTimeKey, otkSig);
    };
    if (!claimAndVerify(false) && !claimAndVerify(true)) {
        LOG(LogChannel::E2EE, "sendOlmToDevice: OTK sig INVALID for %s/%s after re-claim — "
            "its OTK pool holds keys from an older identity (peer must rotate keys)",
            targetUserId.c_str(), targetDeviceId.c_str());
        return false;
    }

    std::string plaintext = "{\"type\":\"" + innerType + "\",\"content\":" + innerContent
        + ",\"sender\":\"" + ctxUserId_ + "\""
        + ",\"recipient\":\"" + targetUserId + "\""
        + ",\"keys\":{\"ed25519\":\"" + ed25519Key() + "\"}"
        + ",\"recipient_keys\":{\"ed25519\":\"" + theirEd + "\"}}";

    // 2b. Reuse an established session when the peer's identity key is
    // unchanged — one OTK per (user, device) ever instead of one per send
    // (fresh pre-key messages drained the peer's OTK pool). The key-request
    // path forces fresh sessions so a peer that evicted ours still creates
    // a matching inbound one.
    std::string cacheKey = targetUserId + "|" + targetDeviceId;
    if (!forceFresh) {
        std::lock_guard<std::mutex> lk(outboundOlmMtx_);
        auto it = outboundOlmSessions_.find(cacheKey);
        if (it != outboundOlmSessions_.end() && it->second.curve == theirCurve) {
            auto encResult = it->second.session->encrypt(plaintext);
            if (encResult.success && !encResult.data.empty()) {
                std::string sendBody = "{\"messages\":{\"" + targetUserId + "\":{\"" + targetDeviceId
                    + "\":{\"algorithm\":\"m.olm.v1.curve25519-aes-sha2\","
                    + "\"ciphertext\":{\"" + theirCurve + "\":{\"body\":\"" + encResult.data
                    + "\",\"type\":" + std::to_string(encResult.messageType) + "}},"
                    + "\"sender_key\":\"" + curve25519Key() + "\"}}}}";
                std::string txnId = "pdolm" + std::to_string(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                std::string url = ctxHomeserver_ + "/_matrix/client/v3/sendToDevice/m.room.encrypted/" + txnId;
                auto resp = httpPut(url, sendBody, hdrs, 15000);
                LOG(LogChannel::E2EE, "sendOlmToDevice: %s to=%s/%s reuse ok=%d status=%d",
                    innerType.c_str(), targetUserId.c_str(), targetDeviceId.c_str(),
                    resp.success ? 1 : 0, resp.statusCode);
                if (resp.success) return true;
            }
            // Peer-side session lost (or encryption failed) — drop and fall
            // through to a fresh pre-key session.
            outboundOlmSessions_.erase(it);
        }
    }

    // 3. Create outbound session + encrypt the inner event.
    auto session = std::make_unique<progressive::OlmSession>();
    auto* acc = static_cast<progressive::OlmAccount*>(account_->rawAccount());
    auto sessResult = session->createOutbound(*acc, theirCurve, oneTimeKey);
    if (!sessResult.success) return false;

    auto encResult = session->encrypt(plaintext);
    if (!encResult.success || encResult.data.empty()) return false;

    // 4. Build + send m.room.encrypted to-device body.
    std::string sendBody = "{\"messages\":{\"" + targetUserId + "\":{\"" + targetDeviceId
        + "\":{\"algorithm\":\"m.olm.v1.curve25519-aes-sha2\","
        + "\"ciphertext\":{\"" + theirCurve + "\":{\"body\":\"" + encResult.data
        + "\",\"type\":" + std::to_string(encResult.messageType) + "}},"
        + "\"sender_key\":\"" + curve25519Key() + "\"}}}}";
    std::string txnId = "pdolm" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::string url = ctxHomeserver_ + "/_matrix/client/v3/sendToDevice/m.room.encrypted/" + txnId;
    auto resp = httpPut(url, sendBody, hdrs, 15000);
    LOG(LogChannel::E2EE, "sendOlmToDevice: %s to=%s/%s ok=%d status=%d",
        innerType.c_str(), targetUserId.c_str(), targetDeviceId.c_str(),
        resp.success ? 1 : 0, resp.statusCode);
    if (resp.success) {
        std::lock_guard<std::mutex> lk(outboundOlmMtx_);
        outboundOlmSessions_[cacheKey] = OutboundOlmTarget{std::move(session), theirCurve};
        return true;
    }
    return false;
}


void Decryptor::forceNewOlmSession(const std::string& senderId, const std::string& senderKey) {
    if (ctxHomeserver_.empty() || ctxToken_.empty()) return;

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    {
        std::lock_guard<std::mutex> lk(requestMtx_);
        auto it = forcedOlm_.find(senderKey);
        if (it != forcedOlm_.end() && nowMs - it->second < 600000) return;
        forcedOlm_[senderKey] = nowMs;
        LOG(LogChannel::E2EE, "forceNewOlmSession: re-establishing Olm session with sender=%.40s (10min window)",
            senderId.c_str());
        // The peer's session is broken — re-arm our pending key requests for
        // this sender so they re-fire soon after the peer rotates (the backoff
        // cap would otherwise keep them silent forever).
        for (auto& [key, st] : requestedKeys_) {
            auto sep = key.rfind('|');
            if (sep != std::string::npos && key.compare(sep + 1, std::string::npos, senderKey) == 0) {
                st.attempts = 1;
                st.lastMs = nowMs - 30001;  // fires on the next maybeReRequestKeys tick
            }
        }
    }

    auto hdrs = makeAuthHeaders(ctxToken_);
    auto ourCurve = curve25519Key();
    auto ourEd = ed25519Key();

    std::string queryBody = "{\"device_keys\":{\"" + senderId + "\":[]}}";
    auto queryResp = httpPost(ctxHomeserver_ + "/_matrix/client/v3/keys/query",
                               queryBody, hdrs, 30000);
    if (!queryResp.success) {
        std::fprintf(stderr, "[e2ee] forceNewOlmSession: keys/query failed for %s status=%d err=%s\n",
                     senderId.c_str(), queryResp.statusCode, queryResp.errorMessage.c_str());
        return;
    }
    simdjson::dom::parser parser;
    auto root = parser.parse(queryResp.body);
    if (root.error() != simdjson::SUCCESS) return;
    std::string theirDeviceId, theirEd;
    auto dkResult = root.value()["device_keys"].get_object();
    if (dkResult.error() == simdjson::SUCCESS) {
        for (auto userField : dkResult.value()) {
            auto userDevices = userField.value.get_object();
            if (userDevices.error() != simdjson::SUCCESS) continue;
            for (auto devField : userDevices.value()) {
                std::string devId(devField.key);
                if (devId == ctxDeviceId_) continue;
                auto keysResult = devField.value["keys"].get_object();
                if (keysResult.error() != simdjson::SUCCESS) continue;
                std::string devCurve, devEd;
                for (auto k : keysResult.value()) {
                    std::string kKey(k.key);
                    if (kKey.find("curve25519") != std::string::npos) {
                        auto v = k.value.get_string();
                        if (v.error() == simdjson::SUCCESS) devCurve = std::string(v.value());
                    }
                    if (kKey.find("ed25519") != std::string::npos) {
                        auto v = k.value.get_string();
                        if (v.error() == simdjson::SUCCESS) devEd = std::string(v.value());
                    }
                }
                if (devCurve == senderKey) {
                    theirDeviceId = devId;
                    theirEd = devEd;
                    break;
                }
            }
        }
    }
    if (theirDeviceId.empty() || theirEd.empty()) {
        std::fprintf(stderr, "[e2ee] forceNewOlmSession: no device found for senderKey=%.20s\n",
                     senderKey.c_str());
        return;
    }

    std::string claimBody = "{\"one_time_keys\":{\""
        + senderId + "\":{\"" + theirDeviceId + "\":\"signed_curve25519\"}}}";
    auto claimResp = httpPost(ctxHomeserver_ + "/_matrix/client/v3/keys/claim",
                               claimBody, hdrs, 15000);
    if (!claimResp.success) {
        std::fprintf(stderr, "[e2ee] forceNewOlmSession: keys/claim failed\n");
        return;
    }
    std::string oneTimeKey;
    auto claimRoot = parser.parse(claimResp.body);
    if (claimRoot.error() == simdjson::SUCCESS) {
        auto otkResult = claimRoot.value()["one_time_keys"].get_object();
        if (otkResult.error() == simdjson::SUCCESS) {
            for (auto userField : otkResult.value()) {
                auto userDevs = userField.value.get_object();
                if (userDevs.error() != simdjson::SUCCESS) continue;
                for (auto devField : userDevs.value()) {
                    auto keyObj = devField.value.get_object();
                    if (keyObj.error() != simdjson::SUCCESS) continue;
                    for (auto k : keyObj.value()) {
                        oneTimeKey = domGetString(k.value, "key");
                        if (oneTimeKey.empty()) {
                            auto keyStr = k.value.get_string();
                            if (keyStr.error() == simdjson::SUCCESS)
                                oneTimeKey = std::string(keyStr.value());
                        }
                        if (!oneTimeKey.empty()) break;
                    }
                }
            }
        }
    }
    if (oneTimeKey.empty()) {
        std::fprintf(stderr, "[e2ee] forceNewOlmSession: no OTK claimed for %s/%s\n",
                     senderId.c_str(), theirDeviceId.c_str());
        return;
    }

    progressive::OlmSession session;
    auto* underlyingAccount = static_cast<progressive::OlmAccount*>(account_->rawAccount());
    auto sessResult = session.createOutbound(*underlyingAccount, senderKey, oneTimeKey);
    if (!sessResult.success) {
        std::fprintf(stderr, "[e2ee] forceNewOlmSession: createOutbound failed\n");
        return;
    }

    std::string plaintext = "{\"type\":\"m.dummy\",\"content\":{},"
        "\"sender\":\"" + ctxUserId_ + "\","
        "\"recipient\":\"" + senderId + "\","
        "\"keys\":{\"ed25519\":\"" + ourEd + "\"},"
        "\"recipient_keys\":{\"ed25519\":\"" + theirEd + "\"}}";

    auto encResult = session.encrypt(plaintext);
    if (!encResult.success || encResult.data.empty()) {
        std::fprintf(stderr, "[e2ee] forceNewOlmSession: Olm encrypt failed\n");
        return;
    }

    // NOTE: the new OUTBOUND session must NOT be stored in olmSessions_ (the
    // inbound store) — an outbound pickle can never decrypt the peer's
    // messages, and it poisoned every subsequent type-1 attempt (BAD_MAC).

    std::string txnId = "pddmy" + std::to_string(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
        + g_txnCounter.fetch_add(1));
    std::ostringstream sendBody;
    sendBody << "{\"messages\":{\""
             << senderId << "\":{\""
             << theirDeviceId << "\":{\"algorithm\":\"m.olm.v1.curve25519-aes-sha2\","
             << "\"ciphertext\":{\"" << senderKey << "\":{"
             << "\"body\":\"" << encResult.data << "\","
             << "\"type\":0}},"
             << "\"sender_key\":\"" << ourCurve << "\"}}}}";
    std::string url = ctxHomeserver_ + "/_matrix/client/v3/sendToDevice/m.room.encrypted/" + txnId;
    auto sendResp = httpPut(url, sendBody.str(), hdrs, 15000);
    std::fprintf(stderr, "[e2ee] forceNewOlmSession: sent m.dummy to %s/%s ok=%d status=%d err=%s\n",
                 senderId.c_str(), theirDeviceId.c_str(), sendResp.success ? 1 : 0,
                 sendResp.statusCode, sendResp.errorMessage.c_str());
    LOG(LogChannel::E2EE, "forceNewOlmSession: sent m.dummy to %s/%s ok=%d status=%d err=%s",
        senderId.c_str(), theirDeviceId.c_str(), sendResp.success ? 1 : 0,
        sendResp.statusCode, sendResp.errorMessage.c_str());
}

std::string Decryptor::pickleOlmSessions(const std::string& key) {
    std::lock_guard<std::mutex> lk(olmMtx_);
    if (olmSessions_.empty()) return "[]";
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& [senderKey, pickles] : olmSessions_) {
        for (const auto& pickle : pickles) {
            if (!first) os << ",";
            first = false;
            os << "{\"k\":\"" << senderKey << "\",\"v\":\"";
            for (unsigned char c : pickle) {
                static const char hex[] = "0123456789abcdef";
                os << hex[c >> 4] << hex[c & 15];
            }
            os << "\"}";
        }
    }
    os << "]";
    return os.str();
}

bool Decryptor::unpickleOlmSessions(const std::string& key, const std::string& data) {
    std::lock_guard<std::mutex> lk(olmMtx_);
    olmSessions_.clear();
    if (data.empty() || data == "[]") return true;
    (void)key;
    // Parse JSON array with simdjson (fixes bug #11: manual brace-matching
    // parser used find("}}") which never matched single-} object endings,
    // causing only the first Olm session to be loaded)
    simdjson::dom::parser parser;
    auto root = parser.parse(data);
    if (root.error() != simdjson::SUCCESS) return true;
    auto arr = root.value().get_array();
    if (arr.error() != simdjson::SUCCESS) return true;
    for (auto elem : arr.value()) {
        auto obj = elem.get_object();
        if (obj.error() != simdjson::SUCCESS) continue;
        auto k = obj.value()["k"].get_string();
        auto v = obj.value()["v"].get_string();
        if (k.error() != simdjson::SUCCESS || v.error() != simdjson::SUCCESS) continue;
        std::string senderKey(k.value());
        std::string hexPickle(v.value());
        if (hexPickle.size() % 2 != 0) continue;
        std::string pickle;
        for (size_t i = 0; i < hexPickle.size(); i += 2) {
            char h = (char)strtol(hexPickle.substr(i, 2).c_str(), nullptr, 16);
            pickle += h;
        }
        auto& vec = olmSessions_[senderKey];
        bool dup = false;
        for (const auto& existing : vec) {
            if (existing == pickle) { dup = true; break; }
        }
        if (!dup && vec.size() < 20) {
            vec.push_back(pickle);
            std::fprintf(stderr, "[e2ee] olm: loaded session %.30s (pickleLen=%zu)\n",
                         senderKey.c_str(), pickle.size());
        } else if (dup) {
        } else {
            LOG(LogChannel::E2EE, "olm: cap reached for sender=%.30s, skipping",
                senderKey.c_str());
        }
    }
    size_t total = 0;
    for (const auto& [k, v] : olmSessions_) total += v.size();
    std::fprintf(stderr, "[e2ee] loaded %zu olm session pickles (%zu senders)\n", total, olmSessions_.size());
    return true;
}

std::string Decryptor::pickleOutboundSessions(const std::string& key) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    if (outboundSessions_.empty()) return "[]";
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& [roomId, out] : outboundSessions_) {
        auto* olmSession = static_cast<::OlmOutboundGroupSession*>(out.session);
        if (!olmSession) continue;
        if (!first) os << ",";
        first = false;
        os << "{\"roomId\":\"" << roomId << "\","
           << "\"sessionId\":\"" << out.sessionId << "\","
           << "\"sessionKey\":\"" << out.sessionKey << "\","
           << "\"messageIndex\":" << out.messageIndex << ","
           << "\"shared\":" << (roomKeysShared_[roomId] ? "true" : "false");
        size_t len = olm_pickle_outbound_group_session_length(olmSession);
        if (len > 0) {
            std::vector<uint8_t> pickled(len);
            size_t ret = olm_pickle_outbound_group_session(olmSession, "", 0, pickled.data(), len);
            if (ret != olm_error()) {
                os << ",\"pickle\":\"";
                for (size_t i = 0; i < len; i++) {
                    static const char hex[] = "0123456789abcdef";
                    os << hex[pickled[i] >> 4] << hex[pickled[i] & 15];
                }
                os << "\"";
            }
        }
        os << "}";
    }
    os << "]";
    return os.str();
}

bool Decryptor::unpickleOutboundSessions(const std::string& key, const std::string& data) {
    std::lock_guard<std::mutex> lk(outboundMtx_);
    outboundSessions_.clear();
    roomKeysShared_.clear();
    if (data.empty() || data == "[]") return true;
    (void)key;
    simdjson::dom::parser parser;
    auto root = parser.parse(data);
    if (root.error() != simdjson::SUCCESS) return false;
    auto arr = root.value().get_array();
    if (arr.error() != simdjson::SUCCESS) return false;
    for (auto elem : arr.value()) {
        auto obj = elem.get_object();
        if (obj.error() != simdjson::SUCCESS) continue;
        auto r = obj.value()["roomId"].get_string();
        auto si = obj.value()["sessionId"].get_string();
        auto sk = obj.value()["sessionKey"].get_string();
        auto mi = obj.value()["messageIndex"].get_int64();
        auto sh = obj.value()["shared"].get_bool();
        auto pk = obj.value()["pickle"].get_string();
        if (r.error() != simdjson::SUCCESS || si.error() != simdjson::SUCCESS ||
            sk.error() != simdjson::SUCCESS) continue;
        std::string roomId(r.value());
        std::string hexPickle(pk.error() == simdjson::SUCCESS ? pk.value() : "");
        if (hexPickle.empty() || hexPickle.size() % 2 != 0) continue;
        std::vector<uint8_t> pickledData;
        for (size_t i = 0; i < hexPickle.size(); i += 2) {
            char h = (char)strtol(hexPickle.substr(i, 2).c_str(), nullptr, 16);
            pickledData.push_back((uint8_t)h);
        }
        // MUST pass a copy — libolm consumes the pickled buffer in place (Quirk 8)
        std::vector<uint8_t> pickledCopy = pickledData;
        void* mem = malloc(olm_outbound_group_session_size());
        if (!mem) continue;
        // olm_outbound_group_session zeros the struct (Quirk 7) — call ONCE
        auto* olmSession = olm_outbound_group_session(mem);
        size_t ret = olm_unpickle_outbound_group_session(olmSession, "", 0,
            pickledCopy.data(), pickledCopy.size());
        if (ret == olm_error()) {
            free(mem);
            continue;
        }
        OutboundMegolmSession out;
        out.session = mem;
        out.sessionId = std::string(si.value());
        out.sessionKey = std::string(sk.value());
        out.messageIndex = mi.error() == simdjson::SUCCESS ? (int)mi.value() : 0;
        outboundSessions_[roomId] = std::move(out);
        bool shared = sh.error() == simdjson::SUCCESS ? sh.value() : false;
        roomKeysShared_[roomId] = shared;
        // DEBT(E2EE): stale shared flag if members changed while offline — new
        // members won't get the room_key until session rotation. matrix-rust-sdk
        // re-shares on device_lists:changed; we don't yet. See AGENTS.md gaps.
        LOG(LogChannel::E2EE, "unpickleOutbound: room=%s sid=%.20s shared=%d",
            roomId.c_str(), out.sessionId.c_str(), shared ? 1 : 0);
    }
    return true;
}

} // namespace progressive::desktop

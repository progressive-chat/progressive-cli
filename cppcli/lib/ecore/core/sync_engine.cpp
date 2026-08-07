// src/core/sync_engine.cpp

#include "sync_engine.hpp"

#include <chrono>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <ctime>
#include "core/debug_log.hpp"
#include "core/thread_pool.hpp"
#include "core/crypto/cross_sign.hpp"
#include "core/crypto/key_backup.hpp"
#include "core/crypto/recovery_key.hpp"
#include "core/crypto/ssss.hpp"
#include <openssl/evp.h>
#include <simdjson.h>

namespace progressive::desktop {

SyncEngine::SyncEngine() = default;

SyncEngine::~SyncEngine() {
    stop();
}

void SyncEngine::initVerificationManager() {
    if (!client_) return;
    // Read the account LIVE inside the fns (AGENTS.md: never cache mutable
    // credentials — the user/device can change on account switch).
    verificationManager_.setOurMasterKeyFn([this]() {
        if (!store_ || !client_) return std::string();
        auto xs = store_->loadCrossSigningKeys(client_->account().userId);
        if (!xs.has_value()) return std::string();
        simdjson::dom::parser p;
        auto d = p.parse(*xs);
        if (d.error() != simdjson::SUCCESS) return std::string();
        auto mp = d.value()["master"]["pub"].get_string();
        return mp.error() == simdjson::SUCCESS ? std::string(mp.value()) : std::string();
    });
    verificationManager_.setTheirMasterKeyFn([this](const std::string& otherUserId) {
        if (!client_) return std::string();
        std::string body = "{\"device_keys\":{\"" + otherUserId + "\":[]}}";
        auto q = client_->queryKeys(body);
        if (!q.ok) return std::string();
        simdjson::dom::parser p;
        auto d = p.parse(q.data);
        if (d.error() != simdjson::SUCCESS) return std::string();
        auto keysObj = d.value()["master_keys"][otherUserId]["keys"].get_object();
        if (keysObj.error() != simdjson::SUCCESS) return std::string();
        for (auto [k, v] : keysObj.value()) {
            std::string kStr(k);
            if (kStr.find("ed25519:") == 0) {
                auto vs = v.get_string();
                if (vs.error() == simdjson::SUCCESS) return std::string(vs.value());
            }
        }
        return std::string();
    });
    LOG(LogChannel::E2EE, "initVerificationManager: MSK exchange fns wired");
}

void SyncEngine::start() {
    LOG(LogChannel::DBG, "sync start called");
    if (running_.exchange(true)) return;  // already running

    // Load this account's saved since-token (per-user — never another
    // account's sync position).
    if (store_ && client_) {
        auto tok = store_->loadSyncToken(client_->account().userId);
        if (tok) sinceToken_ = *tok;
    }
    firstRun_ = true;  // next sync uses empty since → gets current state for all rooms

    worker_ = std::thread([this] { run(); });
}

void SyncEngine::stop() {
    // Always set running_ and detach the worker thread.
    // Can't early-return on !exchange→false because authErrCb_ already
    // sets running_=false — if we return here, worker_ stays joinable
    // and ~thread() calls std::terminate().
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SyncEngine::pause() {
    paused_ = true;
    cv_.notify_all();
}

void SyncEngine::resume() {
    paused_ = false;
    cv_.notify_all();
}

void SyncEngine::setState(SyncEngineState s) {
    stats_.state = s;
    if (stateCb_) stateCb_(s, stats_);
}

int SyncEngine::computeBackoffMs(int consecutiveErrors) const {
    // Exponential backoff capped at 60s. 1s, 2s, 4s, 8s, 16s, 32s, 60s.
    int base = 1000 << std::min(consecutiveErrors, 6);
    return std::min(base, 60000);
}

void SyncEngine::run() {
    setState(sinceToken_.empty() ? SyncEngineState::InitialSync
                                  : SyncEngineState::Running);

    otkCountSeen_ = false;  // per login — servers without count fields get the fallback
    int tokenFailures = 0;

    while (running_) {
        // Pause gate
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return !paused_ || !running_; });
        }
        if (!running_) break;

        // First sync after start(): use empty since token even if we have a saved
        // one. This tells the server "give me current state for all rooms" WITHOUT
        // the massive overhead of full_state=true (which sends ALL historical state
        // events). With empty since + full_state=false, the server returns one copy
        // of each current state event — enough for room names, avatars, encryption.
        // Subsequent syncs use the real sinceToken_ for efficient incremental sync.
        // Timeout: 15s for initial sync (more data), 10s for incremental.
        bool useEmptySince = firstRun_;
        std::string since = useEmptySince ? "" : sinceToken_;
        int timeout = useEmptySince ? 15000 : syncTimeoutMs_;
        auto result = client_->syncFast(since, timeout, false);

        if (!result.ok) {
            stats_.errors++;
            stats_.lastError = result.error.message.empty()
                ? result.error.code
                : result.error.message;

            // Detailed logging for token errors — helps diagnose why sessions
            // expire unexpectedly. Captures: timestamp, error code, HTTP status,
            // error message, since token, our user ID.
            std::fprintf(stderr, "[session] ERROR at %ld: code=%s http=%d msg=%s\n",
                         std::time(nullptr),
                         result.error.code.c_str(),
                         result.httpStatus,
                         result.error.message.c_str());
            std::fprintf(stderr, "[session]   since_token=%s user=%s\n",
                         sinceToken_.substr(0, 20).c_str(),
                         client_ ? client_->account().userId.c_str() : "(null)");

            // Detect invalid access token.
            if (result.error.code == "M_UNKNOWN_TOKEN") {
                tokenFailures++;
                LOG(LogChannel::DBG, "M_UNKNOWN_TOKEN — attempt %d/3", tokenFailures);
                if (tokenFailures >= 3) {
                    LOG(LogChannel::DBG, "M_UNKNOWN_TOKEN repeated %d times — forcing auth error",
                        tokenFailures);
                    setState(SyncEngineState::Stopped);
                    LOG(LogChannel::DBG, "calling authErrCb_ (token loop guard)");
                    if (authErrCb_) authErrCb_();
                    running_ = false;
                    break;
                }

                std::fprintf(stderr, "[session] M_UNKNOWN_TOKEN — access token is invalid.\n"
                                     "  Possible causes:\n"
                                     "    1. Token expired (rare — Synapse doesn't expire by default)\n"
                                     "    2. Password was changed\n"
                                     "    3. Logged out from another client with this device_id\n"
                                     "    4. Server-side token cleanup\n"
                                     "    5. SQLite session.db was corrupted and token is garbage\n");

                auto acct = client_->account();
                std::fprintf(stderr, "[session]   user=%s device=%s refresh=%s\n",
                             acct.userId.c_str(),
                             acct.deviceId.c_str(),
                             acct.refreshToken.empty() ? "(none)"
                                 : (acct.refreshToken.substr(0, 8) + "...").c_str());

                // Retry once — may be a transient network error
                std::fprintf(stderr, "[session]   retrying sync once (transient check)...\n");
                auto retry = client_->syncFast(since, timeout, false);
                if (retry.ok) {
                    std::fprintf(stderr, "[session]   retry OK — false alarm, continuing\n");
                    sinceToken_ = std::string(retry.data.nextBatch);
                    stats_.errors = 0;
                    stats_.syncs++;
                    if (syncCb_) syncCb_(retry.data);
                    continue;
                }

                // Try refresh token if available
                if (client_ && !client_->account().refreshToken.empty()) {
                    LOG(LogChannel::E2EE, "sync /refresh: refreshToken len=%zu",
                        client_->account().refreshToken.size());
                    std::fprintf(stderr, "[session]   trying /refresh with refresh token...\n");
                    auto refresh = client_->refreshAccessToken(client_->account().refreshToken);
                    if (refresh.httpStatus == 200 && !refresh.data.accessToken.empty()) {
                        std::fprintf(stderr, "[session]   /refresh OK — new access token obtained\n");
                        AccountInfo newAcct = client_->account();
                        newAcct.accessToken = refresh.data.accessToken;
                        if (!refresh.data.refreshToken.empty())
                            newAcct.refreshToken = refresh.data.refreshToken;
                        client_->setAccount(newAcct);
                        decryptor_.setCryptoContext(newAcct.userId, newAcct.deviceId,
                                                      newAcct.homeserverUrl, newAcct.accessToken);
                    client_->persistSession();
                    continue;  // retry sync with new token
                    }
                    std::fprintf(stderr, "[session]   /refresh FAILED: %s\n",
                                 refresh.error.message.c_str());
                }

                if (client_ && backupPathProvider_) {
                    std::string backupDir = backupPathProvider_();
                    if (!backupDir.empty()) {
                        std::error_code ec;
                        std::filesystem::create_directories(backupDir, ec);
                        if (!ec) {
                            auto acct = client_->account();
                            std::string filename = acct.userId + "_" +
                                std::to_string(std::time(nullptr)) + ".session";
                            std::ofstream backup(backupDir + filename);
                            if (backup) {
                                backup << "user_id=" << acct.userId << "\n"
                                       << "device_id=" << acct.deviceId << "\n"
                                       << "homeserver=" << acct.homeserverUrl << "\n"
                                       << "refresh_token=" << acct.refreshToken << "\n";
                            }
                        }
                    }
                }

                setState(SyncEngineState::Stopped);
                LOG(LogChannel::DBG, "calling authErrCb_ (fallback after /refresh fail)");
                if (authErrCb_) authErrCb_();
                running_ = false;
                break;
            }

            setState(SyncEngineState::Backoff);

            int backoff = computeBackoffMs(stats_.errors);
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, std::chrono::milliseconds(backoff),
                          [this] { return !running_; });
            continue;
        }

        // Success — update token + stats.
        firstRun_ = false;  // only clear on SUCCESS — retries must still use empty since
        tokenFailures = 0;
        stats_.errors = 0;
        stats_.syncs++;
        sinceToken_ = std::string(result.data.nextBatch);

        // Key backup: re-upload when new sessions arrived (cheap, idempotent).
        maybeUploadBackup();

        stats_.roomsJoined += static_cast<int>(result.data.joinedRooms.size());
        stats_.invites     += static_cast<int>(result.data.invitedRooms.size());

        // Feed m.room.encryption state (rotation policy) to the decryptor.
        for (const auto& [rid, room] : result.data.joinedRooms) {
            if (!room.isEncrypted) continue;
            for (const auto& se : room.stateEvents) {
                if (se.type == "m.room.encryption") {
                    decryptor_.setRoomEncryptionConfig(
                        std::string(rid), std::string(se.contentJson));
                    break;
                }
            }
        }
        stats_.timelineEvents += result.data.totalTimelineEvents;
        stats_.toDeviceEvents += result.data.toDeviceEvents;

        // Share-on-join + device-list re-share: members who join an encrypted
        // room (or whose devices change) must get the CURRENT outbound megolm
        // session key — otherwise everything we send with pre-existing
        // sessions stays unreadable for them (Element shares on membership
        // change; this is the same behavior).
        handleRoomKeyShares(result.data);
        // Process to-device events (E2EE): m.room_key adds megolm sessions,
        // m.room.encrypted handles Olm 1:1 (decrypts room_key delivery).
        processToDeviceEvents(result.data);
        // Room-key request retries (backoff schedule) — every sync tick, even
        // when this sync carried no data (quiet room = no handler call).
        decryptor_.maybeReRequestKeys();
        // Periodic crypto persistence: sessions gained since the last clean
        // close must survive a crash (a power-off otherwise loses them, and a
        // peer's next Olm message fails with BAD_MESSAGE_MAC forever).
        if (stats_.syncs % 20 == 0) persistCrypto();
        if (!running_) break;

        if (!result.data.deviceListChanged.empty()) {
            decryptor_.markDevicesStale(result.data.deviceListChanged);
            LOG(LogChannel::E2EE, "device_lists: marked %zu users as stale",
                result.data.deviceListChanged.size());
        }
        if (!running_) break;

        // Persist token.
        if (store_ && client_ && !sinceToken_.empty()) {
            store_->saveSyncToken(client_->account().userId, sinceToken_);
        }

        // Emit to UI thread.
        if (syncCb_) syncCb_(result.data);

        // Update OTK count tracking from sync response
        if (!running_) break;
        if (result.data.signedCurve25519Count > 0) {
            decryptor_.account()->setUploadedKeyCount(result.data.signedCurve25519Count);
            otkCountSeen_ = true;
        }

        // Auto-upload one-time keys if running low. Homeservers that omit
        // both count fields (some non-Synapse servers) never trigger this —
        // fall back to a periodic self-query (uploadDeviceKeys(true) checks
        // the real count itself and skips when sufficient).
        if (!running_) break;
        if (result.data.signedCurve25519Count >= 0 && result.data.signedCurve25519Count < 50) {
            LOG(LogChannel::E2EE, "sync: OTK count=%d (<50) — uploading fresh keys",
                result.data.signedCurve25519Count);
            uploadDeviceKeys(true);
        } else if (!otkCountSeen_ && stats_.syncs % 20 == 0) {
            uploadDeviceKeys(true);
        }

        // Auto-upload the fallback key if the server reports none unused.
        // Absence of "signed_curve25519" means our fallback was claimed (or
        // never uploaded) — generate + upload a fresh one.
        if (!running_) break;
        if (decryptor_.accountShared()) {
            std::string userId = client_ ? client_->account().userId : "";
            bool hasUnusedFallback = false;
            for (const auto& type : result.data.unusedFallbackKeyTypes) {
                if (type == "signed_curve25519") { hasUnusedFallback = true; break; }
            }
            auto now = std::chrono::steady_clock::now();
            if (hasUnusedFallback) {
                // Server acknowledges our fallback — reset the backoff level.
                fallbackBackoffSecs_[userId] = 0;
            } else {
                // Server never acknowledges: escalate 60s -> 2m -> 5m -> 15m -> 30m -> stop.
                static const int kBackoffLevels[] = {60, 120, 300, 900, 1800};
                static const int kNumLevels = (int)(sizeof(kBackoffLevels)/sizeof(kBackoffLevels[0]));
                static const int kBackoffStopped = -1;
                int& lvl = fallbackBackoffSecs_[userId];
                if (lvl == kBackoffStopped) {
                    // Stopped — only resume when the server confirms the type
                    // (resets above) or the account switches.
                    LOG(LogChannel::E2EE, "sync: fallback backoff stopped (server never acknowledged)");
                } else {
                    int waitSecs = kBackoffLevels[lvl];
                    if (now - lastFallbackUploadAt_[userId] >= std::chrono::seconds(waitSecs)) {
                        LOG(LogChannel::E2EE, "sync: no unused fallback key — uploading (backoff lvl %d, wait %ds)",
                            lvl, waitSecs);
                        uploadFallbackKey();
                        lastFallbackUploadAt_[userId] = now;
                        if (lvl + 1 < kNumLevels) ++lvl;
                        else lvl = kBackoffStopped;  // 30-min level done -> stop
                    }
                }
            }
            // Forget old fallback key 5 min after a successful new one was published
            auto pit = lastFallbackPublishedAt_.find(userId);
            if (pit != lastFallbackPublishedAt_.end() && now - pit->second >= kFallbackForgetDelay) {
                decryptor_.account()->forgetOldFallbackKey();
                LOG(LogChannel::E2EE, "sync: forgot old fallback key (published 5 min ago)");
                lastFallbackPublishedAt_.erase(pit);
            }
        }

        setState(SyncEngineState::Running);
    }

    setState(SyncEngineState::Stopped);
}

// Share the current outbound room key with members who joined an encrypted
// room (timeline + state events), and with members whose devices changed
// (new device = needs the key it never received). The share itself is
// synchronous HTTP, so it runs on the thread pool — the /sync loop must
// never block on it.
void SyncEngine::handleRoomKeyShares(const FastSyncResponse& resp) {
    if (!client_ || !decryptor_.isInitialized()) return;
    auto acct = client_->account();
    ThreadPool::instance().enqueue([this, resp, acct]() {
        doRoomKeyShares(resp, acct);
    });
}

void SyncEngine::doRoomKeyShares(const FastSyncResponse& resp, const AccountInfo& acct) {
    if (!client_ || !decryptor_.isInitialized()) return;
    const std::string ourId = acct.userId;

    auto membershipOf = [](std::string_view contentJson) -> std::string {
        simdjson::dom::parser p;
        auto doc = p.parse(std::string(contentJson));
        if (doc.error() != simdjson::SUCCESS) return {};
        auto m = doc.value()["membership"].get_string();
        if (m.error() != simdjson::SUCCESS) return {};
        return std::string(m.value());
    };

    for (const auto& [rid, room] : resp.joinedRooms) {
        if (!room.isEncrypted) continue;
        const std::string roomId(rid);
        std::vector<FastEvent> joinEvents;
        for (const auto& evt : room.timeline.events) {
            if (evt.type == "m.room.member" && !evt.stateKey.empty() &&
                membershipOf(evt.contentJson) == "join")
                joinEvents.push_back(evt);
        }
        // On the first sync (empty since) the current state arrives in
        // stateEvents — catch members who joined while we were offline.
        // Persisted share markers make this idempotent across restarts.
        for (const auto& se : room.stateEvents) {
            if (se.type == "m.room.member" && !se.stateKey.empty() &&
                membershipOf(se.contentJson) == "join")
                joinEvents.push_back(se);
        }
        if (joinEvents.empty()) continue;

        for (const auto& evt : joinEvents) {
            std::string memberId(evt.stateKey);
            std::string eventId(evt.eventId);
            if (eventId.empty()) continue;
            const bool weJoined = (memberId == ourId);
            // Dedupe by the JOIN EVENT: a member who leaves and re-joins gets
            // a fresh share; sync re-delivery of the same event does not.
            std::string dedupe = roomId + "|" + memberId + "|" + eventId;
            {
                std::lock_guard<std::mutex> lk(shareMtx_);
                if (sharedOnJoin_.count(dedupe)) continue;
            }
            if (store_ && store_->hasRoomKeyShareMarker(ourId, roomId, memberId, eventId))
                continue;

            bool shared = false;
            if (weJoined) {
                // We joined/created an encrypted room: share to every member.
                auto membersResp = client_->getRoomMembers(roomId);
                std::vector<std::string> members;
                if (membersResp.ok) {
                    simdjson::dom::parser mp;
                    auto doc = mp.parse(membersResp.data);
                    if (doc.error() == simdjson::SUCCESS) {
                        auto chunk = doc.value()["chunk"].get_array();
                        if (chunk.error() == simdjson::SUCCESS) {
                            for (auto ev : chunk.value()) {
                                auto mship = ev["content"]["membership"].get_string();
                                if (mship.error() != simdjson::SUCCESS ||
                                    std::string(mship.value()) != "join") continue;
                                auto sk = ev["state_key"].get_string();
                                if (sk.error() == simdjson::SUCCESS)
                                    members.push_back(std::string(sk.value()));
                            }
                        }
                    }
                }
                shared = decryptor_.shareRoomKey(roomId, members, ourId,
                    acct.deviceId, acct.homeserverUrl, acct.accessToken);
                if (shared) {
                    decryptor_.markRoomKeyShared(roomId);
                    // Mark every member's join event so a restart does not
                    // re-share to the whole room again.
                    for (const auto& se : room.stateEvents) {
                        if (se.type == "m.room.member" && !se.stateKey.empty() &&
                            !se.eventId.empty() &&
                            membershipOf(se.contentJson) == "join" && store_)
                            store_->saveRoomKeyShareMarker(ourId, roomId,
                                std::string(se.stateKey), std::string(se.eventId));
                    }
                }
            } else {
                shared = decryptor_.shareRoomKey(roomId, {memberId}, ourId,
                    acct.deviceId, acct.homeserverUrl, acct.accessToken);
            }
            if (shared) {
                std::lock_guard<std::mutex> lk(shareMtx_);
                sharedOnJoin_.insert(dedupe);
                if (store_)
                    store_->saveRoomKeyShareMarker(ourId, roomId, memberId, eventId);
            }
            LOG(LogChannel::E2EE, "share-on-join: room=%.40s member=%s%s shared=%d",
                rid.data(), memberId.c_str(), weJoined ? " (we joined)" : "",
                shared ? 1 : 0);
        }
    }

    // A member's device list changed (new device): re-share the current
    // session so the new device can decrypt. Rate-limited (rare events, but
    // each share claims an OTK per device).
    if (!resp.deviceListChanged.empty()) {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lk(shareMtx_);
            if (nowMs - lastDeviceListShareMs_ < 60000) return;
            lastDeviceListShareMs_ = nowMs;
        }
        for (const auto& [rid, room] : resp.joinedRooms) {
            if (!room.isEncrypted) continue;
            std::vector<std::string> members;
            for (const auto& se : room.stateEvents) {
                if (se.type == "m.room.member" && !se.stateKey.empty() &&
                    membershipOf(se.contentJson) == "join")
                    members.push_back(std::string(se.stateKey));
            }
            if (members.empty()) continue;
            bool shared = decryptor_.shareRoomKey(std::string(rid), members, ourId,
                acct.deviceId, acct.homeserverUrl, acct.accessToken);
            if (shared) decryptor_.markRoomKeyShared(std::string(rid));
            LOG(LogChannel::E2EE, "device-list re-share: room=%.40s members=%zu shared=%d",
                rid.data(), members.size(), shared ? 1 : 0);
        }
    }
}

void SyncEngine::processToDeviceEvents(const FastSyncResponse& resp) {    LOG(LogChannel::E2EE, "processToDevice: %zu toDevice events", resp.toDeviceEventList.size());
    for (const auto& evt : resp.toDeviceEventList) {
        std::fprintf(stderr, "[E2EE] RAW toDevice type='%s' sender='%s' content='%s'\n",
            std::string(evt.type).c_str(), std::string(evt.senderId).c_str(),
            std::string(evt.contentJson).c_str());
        if (evt.type == "m.room_key") {
            std::string contentStr(evt.contentJson);
            LOG(LogChannel::E2EE, "processToDevice: got m.room_key from=%s content=[%.200s]",
                std::string(evt.senderId).c_str(), contentStr.c_str());
            if (decryptor_.handleRoomKey(contentStr, std::string(evt.senderId))) {
                LOG(LogChannel::E2EE, "processToDevice: handleRoomKey OK");
                stats_.decryptedEvents++;
                std::cerr << "[e2ee] added megolm session (room_key from "
                          << evt.senderId << ")\n";
            } else {
                LOG(LogChannel::E2EE, "processToDevice: handleRoomKey FAILED");
                stats_.decryptErrors++;
                std::cerr << "[e2ee] failed to add room_key from "
                          << evt.senderId << ": " << contentStr << "\n";
            }
        } else if (evt.type == "m.room.encrypted") {
            LOG(LogChannel::E2EE, "processToDevice: got m.room.encrypted (Olm-wrapped) from=%s",
                std::string(evt.senderId).c_str());
            std::string contentStr(evt.contentJson);
            std::string innerPlaintext = decryptor_.handleOlmEncryptedToDevice(
                std::string(evt.senderId), contentStr);
            std::fprintf(stderr, "[E2EE] Olm result: size=%zu first200='%.200s'\n",
                innerPlaintext.size(), innerPlaintext.empty() ? "(empty)" : innerPlaintext.c_str());
            if (!innerPlaintext.empty()) {
                LOG(LogChannel::E2EE, "processToDevice: Olm decrypt OK — dispatching inner type");
                stats_.decryptedEvents++;
                std::cerr << "[e2ee] decrypted Olm 1:1 to-device from "
                          << evt.senderId << " (" << innerPlaintext.size() << " bytes)\n";
                // Element wraps verification messages in Olm — route them to
                // the verification manager (cross-client SAS).
                simdjson::dom::parser pd;
                auto pdoc = pd.parse(innerPlaintext);
                if (pdoc.error() == simdjson::SUCCESS) {
                    auto it = pdoc.value()["type"].get_string();
                    auto icr = pdoc.value()["content"];
                    if (it.error() == simdjson::SUCCESS &&
                        std::string_view(it.value()).find("m.key.verification.") == 0 &&
                        icr.error() == simdjson::SUCCESS) {
                        handleVerificationEvent(std::string(it.value()),
                                                std::string(evt.senderId),
                                                simdjson::to_string(icr.value()));
                    }
                }
            } else {
                LOG(LogChannel::E2EE, "processToDevice: Olm decrypt FAILED or not m.room_key");
                stats_.decryptErrors++;
                std::cerr << "[e2ee] Olm 1:1 decryption failed from "
                          << evt.senderId << "\n";
            }
        } else if (evt.type == "m.forwarded_room_key") {
            decryptor_.handleForwardedRoomKey(std::string(evt.contentJson),
                                              std::string(evt.senderId));
        } else if (evt.type == "m.room_key_request") {
            // Another device asks us to re-share a room key. Verified-only
            // policy is enforced inside handleRoomKeyRequest via the checker.
            decryptor_.handleRoomKeyRequest(std::string(evt.contentJson),
                                            std::string(evt.senderId));
        } else if (evt.type == "m.room_key.withheld") {
            // The sender refused to share a room key (m.no_olm etc.) — parse
            // code/reason and surface it against the matching pending request.
            simdjson::dom::parser wp;
            auto wdoc = wp.parse(std::string(evt.contentJson));
            std::string code, reason, fromDevice, senderKey;
            if (wdoc.error() == simdjson::SUCCESS) {
                auto c = wdoc.value()["code"].get_string();
                if (c.error() == simdjson::SUCCESS) code = std::string(c.value());
                auto r = wdoc.value()["reason"].get_string();
                if (r.error() == simdjson::SUCCESS) reason = std::string(r.value());
                auto fd = wdoc.value()["from_device"].get_string();
                if (fd.error() == simdjson::SUCCESS) fromDevice = std::string(fd.value());
                auto sk = wdoc.value()["sender_key"].get_string();
                if (sk.error() == simdjson::SUCCESS) senderKey = std::string(sk.value());
            }
            LOG(LogChannel::E2EE, "WITHHELD from=%s code=%s reason=%s device=%s",
                std::string(evt.senderId).c_str(), code.c_str(), reason.c_str(),
                fromDevice.c_str());
            if (!senderKey.empty())
                decryptor_.noteWithheld(senderKey, fromDevice, reason.empty() ? code : reason);
        } else if (evt.type.find("m.key.verification.") == 0) {
            LOG(LogChannel::E2EE, "processToDevice: verification event type=%s from=%s",
                std::string(evt.type).c_str(), std::string(evt.senderId).c_str());
            handleVerificationEvent(std::string(evt.type), std::string(evt.senderId),
                                    std::string(evt.contentJson));
        }
    }
    // NOTE: resendAllPendingRequests() is called from handleVerificationEvent
    // (Done) — outstanding key requests are re-asked after a completed SAS.
}

// Route a verification to-device event (plain or Olm-decrypted) into the
// verification manager; on Done, persist the verified device, cross-sign
// their master key, and re-ask all outstanding room-key requests.
void SyncEngine::handleVerificationEvent(const std::string& type,
                                         const std::string& senderId,
                                         const std::string& contentJson) {
    std::string userId = client_ ? client_->account().userId : "";
    std::string deviceId = client_ ? client_->account().deviceId : "";
    auto* vtxn = verificationManager_.handleEvent(
        type, senderId, contentJson,
        userId, deviceId, decryptor_.ed25519Key(), decryptor_.curve25519Key());
    // SAS completed -> persist the other device as verified (key-share policy).
    if (vtxn && vtxn->state == VerificationState::Done && store_) {
                store_->saveVerifiedDevice(vtxn->otherUserId, vtxn->otherDeviceId);
                LOG(LogChannel::E2EE, "processToDevice: recorded verified device %s/%s",
                    vtxn->otherUserId.c_str(), vtxn->otherDeviceId.c_str());
                // Cross-sign their master key with our USK — the SAS mac covered
                // both MSKs, so their master key is now SAS-verified. Skipped for
                // self-verifications: the same-user master sig goes through the
                // server's device-signed self path and is rejected (the MSK
                // exchange is meaningless within one account anyway).
                if (!vtxn->theirMasterKey.empty() && client_ &&
                    vtxn->otherUserId != client_->account().userId) {
                    auto xs = store_->loadCrossSigningKeys(client_->account().userId);
                    if (xs.has_value()) {
                        simdjson::dom::parser p;
                        auto d = p.parse(*xs);
                        if (d.error() == simdjson::SUCCESS) {
                            auto uskPub = d.value()["user"]["pub"].get_string();
                            auto uskPriv = d.value()["user"]["priv"].get_string();
                            if (uskPub.error() == simdjson::SUCCESS &&
                                uskPriv.error() == simdjson::SUCCESS) {
                                std::string content = buildCrossSigningContent(
                                    "m.cross_signing.master", vtxn->theirMasterKey,
                                    std::string(uskPub.value()),
                                    std::string(uskPriv.value()), vtxn->otherUserId,
                                    client_->account().userId);
                                // Body key = the target's bare master pub
                                // (Synapse's _process_other_signatures).
                                std::string sigBody = "{\"" + vtxn->otherUserId
                                    + "\":{\"" + vtxn->theirMasterKey + "\":" + content + "}}";
                                auto up = client_->uploadSignatures(sigBody);
                                bool rejected = !up.ok ||
                                    up.data.find("\"failures\"") != std::string::npos &&
                                    up.data.find("\"failures\":{}") == std::string::npos;
                                LOG(LogChannel::E2EE,
                                    "processToDevice: cross-signed master key of %s ok=%d http=%d %s",
                                    vtxn->otherUserId.c_str(), up.ok ? 1 : 0, up.httpStatus,
                                    rejected ? "(signature rejected by the server — see the failures map)" : "");
                            }
                        }
                    }
                }
            }
        // Element parity: after a completed verification, re-ask every session
        // we still miss (the sender may answer now that trust changed).
        decryptor_.resendAllPendingRequests();
    }

// Upload device keys to the server. Call once at login.
// force=true: bypass otk_uploaded_once flag (used by auto-refresh when count<5).
void SyncEngine::uploadDeviceKeys(bool force) {
    LOG(LogChannel::E2EE, "uploadDeviceKeys: ENTER client=%p isLoggedIn=%d decryptor=%d force=%d",
        (void*)client_.get(),
        client_ ? client_->isLoggedIn() : 0,
        decryptor_.isInitialized() ? 1 : 0,
        force ? 1 : 0);

    if (!client_ || !client_->isLoggedIn()) {
        LOG(LogChannel::E2EE, "uploadDeviceKeys: EXIT — client not ready");
        return;
    }
    if (!decryptor_.isInitialized()) {
        LOG(LogChannel::E2EE, "uploadDeviceKeys: EXIT — decryptor not initialized");
        return;
    }

    // Check if device_keys need uploading (new or recreated account).
    bool needDeviceKeys = !decryptor_.accountShared();

    LOG(LogChannel::E2EE, "uploadDeviceKeys: shared=%d needDeviceKeys=%d",
        decryptor_.accountShared() ? 1 : 0, needDeviceKeys ? 1 : 0);

    if (decryptor_.accountShared() && !force) {
        LOG(LogChannel::E2EE, "uploadDeviceKeys: shared=true, skipping "
            "(OTKs managed by auto-refresh)");
        return;
    }

    std::string userId = client_->account().userId;
    std::string deviceId = client_->account().deviceId;
    if (deviceId.empty()) deviceId = "PROGRESSIVE_DESKTOP";

    int serverCount = decryptor_.account()->uploadedKeyCount();
    int maxKeys = 100;
    int needed = std::max(0, maxKeys - serverCount);
    if (needed == 0 && decryptor_.accountShared() && !needDeviceKeys) {
        LOG(LogChannel::E2EE, "uploadDeviceKeys: OTKs sufficient (count=%d), skipping",
            serverCount);
        return;
    }

    // Always discard old unpublished OTKs before generating new ones.
    // Prevents sequential ID collisions (400 "already exists").
    // DEBT(E2EE): mark_keys_as_published also marks the fallback key published,
    // so a pending-unpublished fallback after a failed uploadFallbackKey is
    // orphaned here. The /sync fallback trigger regenerates it (self-healing).
    decryptor_.markOneTimeKeysPublished();
    LOG(LogChannel::E2EE, "uploadDeviceKeys: discarded old OTKs before generating fresh");

    LOG(LogChannel::E2EE, "uploadDeviceKeys: uploading for %s/%s", userId.c_str(), deviceId.c_str());
    // Cross-sign our device with the SSK if cross-signing is set up.
    std::string sskPriv, sskPub;
    if (store_) {
        auto xs = store_->loadCrossSigningKeys(userId);
        if (xs.has_value()) {
            simdjson::dom::parser p;
            auto d = p.parse(*xs);
            if (d.error() == simdjson::SUCCESS) {
                auto sp = d.value()["self"]["priv"].get_string();
                auto spb = d.value()["self"]["pub"].get_string();
                if (sp.error() == simdjson::SUCCESS) sskPriv = std::string(sp.value());
                if (spb.error() == simdjson::SUCCESS) sskPub = std::string(spb.value());
            }
        }
    }
    std::string body = decryptor_.buildKeysUploadBody(userId, deviceId, needed,
        needDeviceKeys, !decryptor_.accountShared(), sskPriv, sskPub);
    LOG(LogChannel::E2EE, "uploadDeviceKeys: our curve25519=%s ed25519=%s",
        decryptor_.curve25519Key().c_str(),
        decryptor_.ed25519Key().c_str());

    auto result = client_->uploadKeys(body);
    LOG(LogChannel::E2EE, "uploadDeviceKeys: result ok=%d httpStatus=%d bodyLen=%zu",
        result.ok ? 1 : 0, result.httpStatus, body.size());

    if (!result.ok && result.httpStatus == 400 &&
        result.error.message.find("already exists") != std::string::npos) {
        // A fresh account's OTK ids collide with stale keys still stored on
        // the server under this device id (left over from an older identity —
        // /delete_devices 404s on custom servers, so they linger). Discard +
        // regenerate advances the id counter past the collision set.
        LOG(LogChannel::E2EE, "uploadDeviceKeys: 400 'already exists' — retrying once with fresh OTKs");
        decryptor_.markOneTimeKeysPublished();
        body = decryptor_.buildKeysUploadBody(userId, deviceId, needed,
            needDeviceKeys, !decryptor_.accountShared(), sskPriv, sskPub);
        result = client_->uploadKeys(body);
        LOG(LogChannel::E2EE, "uploadDeviceKeys: retry ok=%d httpStatus=%d",
            result.ok ? 1 : 0, result.httpStatus);
    }

    if (result.ok) {
        LOG(LogChannel::E2EE, "uploadDeviceKeys: SUCCESS — response=[%.200s]", result.data.c_str());
        // Mark OTKs as published — they remain usable by olm_create_inbound_session
        // but won't be returned by olm_account_one_time_keys (prevents re-upload).
        decryptor_.markOneTimeKeysPublished();
        if (needDeviceKeys) {
            decryptor_.markAccountAsShared();
            LOG(LogChannel::E2EE, "uploadDeviceKeys: account marked as shared");
        }
        if (needed > 0) {
            decryptor_.account()->setUploadedKeyCount(serverCount + needed);
            LOG(LogChannel::E2EE, "uploadDeviceKeys: OTK count updated locally to %d (was %d, added %d)",
                serverCount + needed, serverCount, needed);
        }
        {
            std::string queryBody = "{\"device_keys\":{\"" + userId + "\":[]}}";
            auto queryResp = client_->queryKeys(queryBody);
            LOG(LogChannel::E2EE, "uploadDeviceKeys: self-query http=%d body=%.800s",
                queryResp.httpStatus,
                queryResp.ok ? queryResp.data.c_str() : "");
        }
    } else {
        LOG(LogChannel::E2EE, "uploadDeviceKeys: FAILED — error=%s (OTKs persisted, will retry via auto-refresh)",
            result.error.message.c_str());
    }

    // Save pickle regardless of upload success — OTKs are in memory now and must
    // be persisted for createInbound to work on restart. (fixes bug #12: 401 race
    // prevented otk_persisted from being set, causing infinite regeneration every
    // restart and eventual OTK eviction from libolm's MAX-100 bounded list)
    {
        std::string pickleKey = userId + "/" + deviceId;
        std::string newPickle = decryptor_.saveAccountPickle(pickleKey);
        if (!newPickle.empty() && store_) {
            store_->saveOlmAccount(newPickle, pickleKey, decryptor_.accountShared(),
                                    decryptor_.account()->uploadedKeyCount());
            LOG(LogChannel::E2EE, "uploadDeviceKeys: account pickle saved (shared=%d, published=%d)",
                decryptor_.accountShared() ? 1 : 0, result.ok ? 1 : 0);
        }
    }
}

// Build the /keys/device_signing/upload body for a key set.
static std::string buildDeviceSigningUploadBody(const progressive::desktop::CrossSigningKeys& keys,
    const std::string& userId, const std::string& authJson) {
    auto master = progressive::desktop::buildCrossSigningContent("m.cross_signing.master",
        keys.masterPub, "", "", userId);
    auto self = progressive::desktop::buildCrossSigningContent("m.cross_signing.self_signing",
        keys.selfPub, keys.masterPub, keys.masterPriv, userId);
    auto user = progressive::desktop::buildCrossSigningContent("m.cross_signing.user_signing",
        keys.userPub, keys.masterPub, keys.masterPriv, userId);
    std::string out = "{";
    if (!authJson.empty()) out += "\"auth\":" + authJson + ",";
    out += "\"master_key\":" + master
        + ",\"self_signing_key\":" + self
        + ",\"user_signing_key\":" + user + "}";
    return out;
}

bool SyncEngine::setupCrossSigning() {
    if (!client_ || !client_->isLoggedIn()) return false;
    std::string userId = client_->account().userId;

    // Already published (the source of truth is the server state).
    if (isCrossSigningPublished(userId)) {
        LOG(LogChannel::E2EE, "setupCrossSigning: already published for %s", userId.c_str());
        return true;
    }

    // Local keys from a pending/abandoned flow? Re-publish (UIA-aware)
    // instead of regenerating — regenerating would invalidate the SSK sigs.
    CrossSigningKeys keys;
    bool haveLocal = false;
    auto stored = store_ ? store_->loadCrossSigningKeys(userId) : std::nullopt;
    if (stored.has_value()) {
        simdjson::dom::parser p;
        auto d = p.parse(*stored);
        if (d.error() == simdjson::SUCCESS) {
            auto g = [&](const char* which, std::string& pub, std::string& priv) {
                auto pp = d.value()[which]["pub"].get_string();
                auto pr = d.value()[which]["priv"].get_string();
                if (pp.error() == simdjson::SUCCESS) pub = std::string(pp.value());
                if (pr.error() == simdjson::SUCCESS) priv = std::string(pr.value());
            };
            g("master", keys.masterPub, keys.masterPriv);
            g("user", keys.userPub, keys.userPriv);
            g("self", keys.selfPub, keys.selfPriv);
            haveLocal = !keys.masterPub.empty();
        }
    }
    if (!haveLocal) {
        keys = generateCrossSigningKeys();
        if (keys.masterPub.empty()) return false;
    }

    int rc = publishCrossSigningKeys(keys, userId, "");
    if (rc == 0) {
        // UIA required — persist the keys so the password retry can use them,
        // and stash the session. Return false (NeedsPassword).
        if (!haveLocal) saveCrossSigningKeysJson(userId, keys);
        LOG(LogChannel::E2EE, "setupCrossSigning: UIA required for %s — awaiting password",
            userId.c_str());
        return false;
    }
    if (rc < 0) return false;

    if (!haveLocal) saveCrossSigningKeysJson(userId, keys);
    reuploadDeviceKeys(userId, keys);
    LOG(LogChannel::E2EE, "setupCrossSigning: keys generated + uploaded for %s",
        userId.c_str());
    return true;
}

std::string SyncEngine::createKeyBackupNow() {
    if (!client_ || !client_->isLoggedIn() || !store_) return "";
    return createKeyBackup(*client_, store_.get(), client_->account().userId);
}

bool SyncEngine::uploadKeyBackupNow() {
    if (!client_ || !client_->isLoggedIn() || !store_) return false;
    auto info = store_->loadBackupInfo(client_->account().userId);
    if (!info.has_value()) return false;
    bool ok = uploadKeyBackup(*client_, decryptor_, *info);
    if (ok) decryptor_.markBackupClean();
    return ok;
}

int SyncEngine::restoreKeyBackupNow(const std::string& recoveryKey) {
    if (!client_ || !client_->isLoggedIn() || !store_) return 0;
    BackupInfo info;
    info.version = "";
    info.recoveryKey = recoveryKey;
    info.publicKey = deriveBackupKey(recoveryKeySeed(recoveryKey)).publicKeyB64;
    if (info.publicKey.empty()) return 0;
    // The version: the LATEST on the server (cross-device restore — a new
    // device has no local store entry). Fallback: the stored version.
    auto versions = client_->getRoomKeysVersions();
    if (versions.ok) {
        // Synapse returns the LATEST version object directly:
        // {"version":"1","algorithm":...,"auth_data":...,"etag":..,"count":N}
        simdjson::dom::parser p;
        auto doc = p.parse(versions.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto v = doc.value()["version"].get_string();
            if (v.error() == simdjson::SUCCESS)
                info.version = std::string(v.value());
        }
    }
    if (info.version.empty()) {
        auto stored = store_->loadBackupInfo(client_->account().userId);
        if (stored.has_value()) info.version = stored->version;
    }
    if (info.version.empty()) return 0;
    return restoreKeyBackup(*client_, decryptor_, info);
}

bool SyncEngine::deleteKeyBackupNow() {
    if (!client_ || !client_->isLoggedIn() || !store_) return false;
    auto userId = client_->account().userId;
    auto info = store_->loadBackupInfo(userId);
    if (info.has_value() && !info->version.empty())
        client_->deleteRoomKeysVersion(info->version);
    store_->clearBackupInfo(userId);
    return true;
}

void SyncEngine::maybeUploadBackup() {
    if (!client_ || !client_->isLoggedIn() || !store_) return;
    if (!decryptor_.backupDirty()) return;
    uploadKeyBackupNow();
}

bool SyncEngine::uploadSsssSecrets(const std::string& recoveryKey) {
    if (!client_ || !client_->isLoggedIn() || !store_) return false;
    auto seed = recoveryKeySeed(recoveryKey);
    if (seed.size() != 32) return false;
    auto userId = client_->account().userId;

    auto xs = store_->loadCrossSigningKeys(userId);
    if (!xs.has_value()) return false;
    std::string masterPriv, selfPriv, userPriv;
    {
        simdjson::dom::parser p;
        auto d = p.parse(*xs);
        if (d.error() != simdjson::SUCCESS) return false;
        auto g = [&](const char* which, std::string& priv) {
            auto pr = d.value()[which]["priv"].get_string();
            if (pr.error() == simdjson::SUCCESS) priv = std::string(pr.value());
        };
        g("master", masterPriv);
        g("self", selfPriv);
        g("user", userPriv);
    }
    if (masterPriv.empty() || selfPriv.empty() || userPriv.empty()) return false;

    std::string keyId = generateSsssKeyId();
    std::vector<uint8_t> aesKey, hmacKey;
    if (!deriveSsssKeys(seed, keyId, aesKey, hmacKey)) return false;

    if (!client_->setAccountData("m.secret_storage.key." + keyId,
            buildSsssKeyMetadata(aesKey, hmacKey)).ok) return false;
    // The spec's discovery: m.secret_storage.default_key names the key id
    // (Synapse does NOT implement the global account-data listing endpoint).
    if (!client_->setAccountData("m.secret_storage.default_key",
            "{\"key\":\"" + keyId + "\"}").ok) return false;
    if (!client_->setAccountData("m.cross_signing.master",
            encryptSsssSecret(masterPriv, aesKey, hmacKey)).ok) return false;
    if (!client_->setAccountData("m.cross_signing.self_signing",
            encryptSsssSecret(selfPriv, aesKey, hmacKey)).ok) return false;
    if (!client_->setAccountData("m.cross_signing.user_signing",
            encryptSsssSecret(userPriv, aesKey, hmacKey)).ok) return false;
    LOG(LogChannel::E2EE, "uploadSsssSecrets: cross-signing secrets encrypted "
        "to account-data (keyId=%s)", keyId.c_str());
    return true;
}

// ed25519 public key from a 32-byte seed via OpenSSL (libsodium's
// crypto_sign_ed25519_seed_keypair SEGFAULTS on some AArch64 builds).
static std::string ed25519PubFromSeed(const std::string& seedB64) {
    auto seed = base64Decode(seedB64);
    if (seed.size() != 32) return "";
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const uint8_t*>(seed.data()), 32);
    if (!pkey) return "";
    std::vector<uint8_t> pub(32);
    size_t len = pub.size();
    if (EVP_PKEY_get_raw_public_key(pkey, pub.data(), &len) != 1 || len != 32) {
        EVP_PKEY_free(pkey);
        return "";
    }
    EVP_PKEY_free(pkey);
    return base64Encode(std::string(pub.begin(), pub.end()));
}

int SyncEngine::retrieveSsssSecrets(const std::string& recoveryKey) {
    if (!client_ || !client_->isLoggedIn() || !store_) return 0;
    auto seed = recoveryKeySeed(recoveryKey);
    if (seed.size() != 32) return 0;
    auto userId = client_->account().userId;

    // Discover the SSSS key id via m.secret_storage.default_key.
    auto defKey = client_->getAccountData("m.secret_storage.default_key");
    if (!defKey.ok) return 0;
    std::string keyId;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(defKey.data);
        if (doc.error() != simdjson::SUCCESS) return 0;
        auto k = doc.value()["key"].get_string();
        if (k.error() != simdjson::SUCCESS) return 0;
        keyId = std::string(k.value());
    }
    if (keyId.empty()) return 0;

    auto meta = client_->getAccountData("m.secret_storage.key." + keyId);
    if (!meta.ok) return 0;
    std::string metadataJson;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(meta.data);
        if (doc.error() != simdjson::SUCCESS) return 0;
        auto ivS = doc.value()["iv"].get_string();
        auto macS = doc.value()["mac"].get_string();
        if (ivS.error() != simdjson::SUCCESS || macS.error() != simdjson::SUCCESS) return 0;
        metadataJson = "{\"iv\":\"" + std::string(ivS.value())
            + "\",\"mac\":\"" + std::string(macS.value()) + "\"}";
    }

    std::vector<uint8_t> aesKey, hmacKey;
    if (!deriveSsssKeys(seed, keyId, aesKey, hmacKey)) return 0;
    if (!verifySsssRecoveryKey(metadataJson, aesKey, hmacKey)) {
        LOG(LogChannel::E2EE, "retrieveSsssSecrets: recovery key does not match "
            "the stored SSSS key metadata");
        return 0;
    }

    auto master = client_->getAccountData("m.cross_signing.master");
    auto self = client_->getAccountData("m.cross_signing.self_signing");
    auto user = client_->getAccountData("m.cross_signing.user_signing");
    if (!master.ok || !self.ok || !user.ok) return 0;

    std::string masterPriv = decryptSsssSecret(master.data, aesKey, hmacKey);
    std::string selfPriv = decryptSsssSecret(self.data, aesKey, hmacKey);
    std::string userPriv = decryptSsssSecret(user.data, aesKey, hmacKey);
    if (masterPriv.empty() || selfPriv.empty() || userPriv.empty()) return 0;

    CrossSigningKeys keys;
    keys.masterPriv = masterPriv;
    keys.selfPriv = selfPriv;
    keys.userPriv = userPriv;
    keys.masterPub = ed25519PubFromSeed(masterPriv);
    keys.selfPub = ed25519PubFromSeed(selfPriv);
    keys.userPub = ed25519PubFromSeed(userPriv);
    if (keys.masterPub.empty() || keys.selfPub.empty() || keys.userPub.empty()) return 0;

    saveCrossSigningKeysJson(userId, keys);
    reuploadDeviceKeys(userId, keys);
    LOG(LogChannel::E2EE, "retrieveSsssSecrets: cross-signing secrets restored "
        "for %s — device keys re-signed with the SSK", userId.c_str());
    return 1;
}


SyncEngine::E2eeInitResult SyncEngine::initializeE2EE() {
    E2eeInitResult result;
    if (!client_ || !client_->isLoggedIn() || !decryptor_.isInitialized()) {
        // A fresh or reloaded account — initializeE2EE() itself does the init.
    }
    auto acct = client_->account();
    std::string pickleKey = acct.userId + "/" + acct.deviceId;

    try {
        std::string savedPickle, savedKey;
        bool savedShared = false;
        int savedKeyCount = 0;
        if (store_) {
            auto saved = store_->loadOlmAccount(pickleKey);
            if (saved) {
                savedPickle = saved->pickle;
                savedKey = saved->pickleKey;
                savedShared = saved->shared;
                savedKeyCount = saved->uploadedKeyCount;
            }
        }
        if (!savedPickle.empty()) {
            result.e2eeOk = decryptor_.init(savedPickle, savedKey, savedShared);
            if (result.e2eeOk) {
                decryptor_.account()->setUploadedKeyCount(savedKeyCount);
            } else {
                LOG(LogChannel::E2EE, "initializeE2EE: failed to load saved olm account — creating new one");
                result.e2eeOk = decryptor_.init();
            }
        } else {
            result.e2eeOk = decryptor_.init();
        }
        if (!result.e2eeOk) {
            LOG(LogChannel::E2EE, "initializeE2EE: failed to create olm account");
            return result;
        }

        // All-A curve25519 = an account corrupted by the old load-over-
        // initialized bug (zeroed identity). Peers can never decrypt our
        // messages and we can never decrypt theirs. Heal automatically:
        // regenerate the identity and re-upload device keys.
        {
            static const std::string kAllASentinel(43, 'A');
            std::string curve = decryptor_.curve25519Key();
            if (curve == kAllASentinel) {
                LOG(LogChannel::E2EE, "initializeE2EE: CORRUPT identity (all-A key) for %s — regenerating",
                    acct.userId.c_str());
                if (decryptor_.resetIdentity()) {
                    decryptor_.setAccountShared(false);
                    uploadDeviceKeys(true);
                    LOG(LogChannel::E2EE, "initializeE2EE: identity regenerated — new curve=%.30s",
                        decryptor_.curve25519Key().c_str());
                }
            }
        }

        decryptor_.setCryptoContext(acct.userId, acct.deviceId,
                                     acct.homeserverUrl, acct.accessToken);
        std::string newPickle = decryptor_.saveAccountPickle(pickleKey);
        if (!newPickle.empty() && store_) {
            store_->saveOlmAccount(newPickle, pickleKey,
                                   decryptor_.accountShared(),
                                   decryptor_.account()->uploadedKeyCount());
        }
        if (store_) {
            auto megolmData = store_->loadMegolmSessions(pickleKey);
            if (megolmData && !megolmData->empty()) {
                decryptor_.megolm()->unpickleAll(pickleKey, *megolmData);
            }
            auto outboundData = store_->loadOutboundSessions(pickleKey);
            if (outboundData) {
                decryptor_.unpickleOutboundSessions(pickleKey, *outboundData);
            }
            auto pendingReqData = store_->loadPendingKeyRequests(pickleKey);
            if (pendingReqData && !pendingReqData->empty())
                decryptor_.unpicklePendingKeyRequests(*pendingReqData);
            auto olmSessionsData = store_->loadOlmSessions(pickleKey);
            if (olmSessionsData && !olmSessionsData->empty()) {
                decryptor_.unpickleOlmSessions(pickleKey, *olmSessionsData);
                if (olmSessionsData->size() > 500000) {
                    std::string trimmed = decryptor_.pickleOlmSessions(pickleKey);
                    store_->saveOlmSessions(trimmed, pickleKey);
                }
            }
        }
        result.keysPublished = true;
        // NOTE: the device-keys upload is NOT enqueued here — the caller owns
        // the thread + lifetime (AGENTS.md: enqueue with a lifetime-safe guard).
        // The UI's session_bootstrap schedules it after initializeE2EE().
    } catch (const std::exception& e) {
        LOG(LogChannel::E2EE, "initializeE2EE: exception %s", e.what());
    }
    return result;
}

void SyncEngine::persistCrypto() {
    // Close-event saves and periodic sync-loop saves must never overlap
    // (the decryptor's session map is touched under its own lock, but the
    // whole pickling sequence must stay serialized).
    std::lock_guard<std::mutex> lk(persistMtx_);
    if (!client_ || !store_ || !decryptor_.isInitialized() || !decryptor_.isInitialized()) return;
    std::string pickleKey = client_->account().userId + "/" + client_->account().deviceId;
    auto megolmPickle = decryptor_.megolm()->pickleAll(pickleKey);
    if (!megolmPickle.empty()) {
        store_->saveMegolmSessions(megolmPickle, pickleKey);
    }
    auto outboundPickle = decryptor_.pickleOutboundSessions(pickleKey);
    if (!outboundPickle.empty() && outboundPickle != "[]") {
        store_->saveOutboundSessions(outboundPickle, pickleKey);
    }
    auto olmSessionsPickle = decryptor_.pickleOlmSessions(pickleKey);
    if (!olmSessionsPickle.empty()) store_->saveOlmSessions(olmSessionsPickle, pickleKey);
    auto pendingPickle = decryptor_.picklePendingKeyRequests();
    if (!pendingPickle.empty() && pendingPickle != "[]")
        store_->savePendingKeyRequests(pendingPickle, pickleKey);
}

bool SyncEngine::resetCrossSigning() {
    if (!client_ || !client_->isLoggedIn()) return false;
    std::string userId = client_->account().userId;

    auto keys = generateCrossSigningKeys();
    if (keys.masterPub.empty()) return false;
    saveCrossSigningKeysJson(userId, keys);
    int rc = publishCrossSigningKeys(keys, userId, "");
    if (rc == 0) {
        LOG(LogChannel::E2EE, "resetCrossSigning: UIA required for %s — awaiting password",
            userId.c_str());
        return false;
    }
    if (rc < 0) return false;
    reuploadDeviceKeys(userId, keys);
    // Old SAS verifications are meaningless against the new keys.
    if (store_) store_->clearVerifiedDevices();
    LOG(LogChannel::E2EE, "resetCrossSigning: keys regenerated + uploaded for %s "
        "(verified_devices cleared)", userId.c_str());
    return true;
}

bool SyncEngine::setupCrossSigningWithPassword(const std::string& password) {
    if (!client_ || !client_->isLoggedIn() || uiaSession_.empty()) return false;
    std::string userId = client_->account().userId;
    auto stored = store_ ? store_->loadCrossSigningKeys(userId) : std::nullopt;
    if (!stored.has_value()) return false;  // no pending keys — run setup first

    CrossSigningKeys keys;
    {
        simdjson::dom::parser p;
        auto d = p.parse(*stored);
        if (d.error() != simdjson::SUCCESS) return false;
        auto g = [&](const char* which, std::string& pub, std::string& priv) {
            auto pp = d.value()[which]["pub"].get_string();
            auto pr = d.value()[which]["priv"].get_string();
            if (pp.error() == simdjson::SUCCESS) pub = std::string(pp.value());
            if (pr.error() == simdjson::SUCCESS) priv = std::string(pr.value());
        };
        g("master", keys.masterPub, keys.masterPriv);
        g("user", keys.userPub, keys.userPriv);
        g("self", keys.selfPub, keys.selfPriv);
    }
    if (keys.masterPub.empty()) return false;

    std::string authJson = "{\"type\":\"m.login.password\",\"identifier\":{"
        "\"type\":\"m.id.user\",\"user\":\"" + userId + "\"},"
        "\"password\":\"" + password + "\",\"session\":\"" + uiaSession_ + "\"}";
    int rc = publishCrossSigningKeys(keys, userId, authJson);
    if (rc == 0) {
        // Stale session? The server issued a fresh challenge — keep the keys,
        // update the session, and let the user retry the password.
        LOG(LogChannel::E2EE, "setupCrossSigningWithPassword: new UIA challenge (stale session?) — retry");
        return false;
    }
    if (rc < 0) {
        // Permanent failure (e.g. wrong password) — clear the pending state so
        // setup can re-run (it re-publishes the same local keys).
        uiaSession_.clear();
        return false;
    }
    uiaSession_.clear();
    reuploadDeviceKeys(userId, keys);
    LOG(LogChannel::E2EE, "setupCrossSigningWithPassword: cross-signing published for %s",
        userId.c_str());
    return true;
}

bool SyncEngine::isCrossSigningPublished(const std::string& userId) {
    if (!client_) return false;
    std::string q = "{\"device_keys\":{\"" + userId + "\":[]}}";
    auto resp = client_->queryKeys(q);
    if (!resp.ok) return false;
    simdjson::dom::parser p;
    auto doc = p.parse(resp.data);
    if (doc.error() != simdjson::SUCCESS) return false;
    auto mk = doc.value()["master_keys"][userId];
    return mk.error() == simdjson::SUCCESS;
}

void SyncEngine::saveCrossSigningKeysJson(const std::string& userId,
    const CrossSigningKeys& keys) {
    if (!store_) return;
    std::string json = "{\"master\":{\"pub\":\"" + keys.masterPub
        + "\",\"priv\":\"" + keys.masterPriv
        + "\"},\"user\":{\"pub\":\"" + keys.userPub
        + "\",\"priv\":\"" + keys.userPriv
        + "\"},\"self\":{\"pub\":\"" + keys.selfPub
        + "\",\"priv\":\"" + keys.selfPriv + "\"}}";
    store_->saveCrossSigningKeys(userId, json);
}

void SyncEngine::reuploadDeviceKeys(const std::string& userId,
    const CrossSigningKeys& keys) {
    if (!client_) return;
    std::string dkBody = decryptor_.buildKeysUploadBody(userId,
        client_->account().deviceId, 0, true, false, keys.selfPriv, keys.selfPub,
        /*omitOneTimeKeys=*/true);
    if (!dkBody.empty()) {
        auto dkUp = client_->uploadKeys(dkBody);
        LOG(LogChannel::E2EE, "setupCrossSigning: device re-upload ok=%d status=%d",
            dkUp.ok ? 1 : 0, dkUp.httpStatus);
    }
}

int SyncEngine::publishCrossSigningKeys(const CrossSigningKeys& keys,
    const std::string& userId, const std::string& authJson) {
    if (!client_) return -1;
    std::string body = buildDeviceSigningUploadBody(keys, userId, authJson);
    auto up = client_->uploadDeviceSigningKeys(body);
    if (up.ok) return 1;
    if (up.httpStatus == 401 && up.data.find("\"session\"") != std::string::npos) {
        simdjson::dom::parser p;
        auto doc = p.parse(up.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto sess = doc.value()["session"].get_string();
            if (sess.error() == simdjson::SUCCESS) uiaSession_ = std::string(sess.value());
        }
        return 0;
    }
    LOG(LogChannel::E2EE, "setupCrossSigning: device_signing/upload FAILED status=%d",
        up.httpStatus);
    return -1;
}



void SyncEngine::uploadFallbackKey() {
    LOG(LogChannel::E2EE, "uploadFallbackKey: ENTER client=%p isLoggedIn=%d decryptor=%d",
        client_.get(), client_ && client_->isLoggedIn() ? 1 : 0,
        decryptor_.isInitialized() ? 1 : 0);

    if (!client_ || !client_->isLoggedIn()) return;
    if (!decryptor_.isInitialized()) return;
    if (!decryptor_.accountShared()) return;

    std::string userId = client_->account().userId;
    std::string deviceId = client_->account().deviceId;
    if (deviceId.empty()) deviceId = "PROGRESSIVE_DESKTOP";

    // Generate a fresh fallback key only if none is unpublished yet —
    // keeps retries idempotent after a failed upload.
    if (decryptor_.account()->unpublishedFallbackKey().empty()) {
        if (!decryptor_.account()->generateFallbackKey()) {
            LOG(LogChannel::E2EE, "uploadFallbackKey: generateFallbackKey FAILED");
            return;
        }
        LOG(LogChannel::E2EE, "uploadFallbackKey: generated fresh fallback key");
    }

    std::string fallbackSection = decryptor_.buildFallbackKeysSection(userId, deviceId);
    if (fallbackSection.empty()) {
        LOG(LogChannel::E2EE, "uploadFallbackKey: no fallback key to upload (section empty)");
        return;
    }

    std::string body = "{\"fallback_keys\":" + fallbackSection + "}";
    auto result = client_->uploadKeys(body);
    LOG(LogChannel::E2EE, "uploadFallbackKey: upload ok=%d httpStatus=%d",
        result.ok ? 1 : 0, result.httpStatus);

    if (result.ok) {
        // markOneTimeKeysPublished marks the fallback key as published too
        // (libolm: mark_keys_as_published covers both OTKs and fallback).
        // DEBT(E2EE): this also marks any unpublished OTKs as published without
        // ever uploading them, orphaning them. The OTK auto-refresh at uploadDeviceKeys
        // discards old unpublished OTKs then regenerates (self-healing).
        decryptor_.markOneTimeKeysPublished();
        LOG(LogChannel::E2EE, "uploadFallbackKey: SUCCESS — fallback published");
        std::string ufUserId = client_ ? client_->account().userId : "";
        if (!ufUserId.empty()) lastFallbackPublishedAt_[ufUserId] = std::chrono::steady_clock::now();
        std::string pickleKey = userId + "/" + deviceId;
        std::string newPickle = decryptor_.saveAccountPickle(pickleKey);
        if (!newPickle.empty() && store_) {
            store_->saveOlmAccount(newPickle, pickleKey, decryptor_.accountShared(),
                                    decryptor_.account()->uploadedKeyCount());
            LOG(LogChannel::E2EE, "uploadFallbackKey: account pickle saved");
        }
    } else {
        // Fallback key stays unpublished — cooldown (60s) prevents hammering,
        // next sync retries with the same key.
        LOG(LogChannel::E2EE, "uploadFallbackKey: FAILED — retry after cooldown, error=%s",
            result.error.message.c_str());
    }
}

} // namespace progressive::desktop

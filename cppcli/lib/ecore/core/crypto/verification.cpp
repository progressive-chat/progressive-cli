// src/core/crypto/verification.cpp
#include "verification.hpp"
#include "random.hpp"
#include "../debug_log.hpp"
#include <simdjson.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <olm/olm.h>

namespace progressive::desktop {

std::string cancelCodeToString(CancelCode code) {
    switch (code) {
        case CancelCode::User: return "m.user";
        case CancelCode::Timeout: return "m.timeout";
        case CancelCode::UnknownTransaction: return "m.unknown_transaction";
        case CancelCode::UnknownMethod: return "m.unknown_method";
        case CancelCode::UnexpectedMessage: return "m.unexpected_message";
        case CancelCode::KeyMismatch: return "m.key_mismatch";
        case CancelCode::UserMismatch: return "m.user_mismatch";
        case CancelCode::InvalidMessage: return "m.invalid_message";
        case CancelCode::Accepted: return "m.accepted";
        case CancelCode::Sasmismatch: return "m.mismatched_sas";
        case CancelCode::Other: return "m.unknown";
    }
    return "m.unknown";
}

bool VerificationTransaction::isExpired() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::minutes>(now - startTime).count() >= 10;
}

std::string VerificationManager::generateTransactionId() {
    uint8_t rnd[16];
    fillCryptoRandom(rnd, sizeof(rnd));
    std::ostringstream os;
    os << "pdv_";
    for (int i = 0; i < 16; i++)
        os << std::hex << std::setw(2) << std::setfill('0') << (int)rnd[i];
    return os.str();
}

VerificationTransaction* VerificationManager::startVerification(
    const std::string& otherUserId, const std::string& otherDeviceId,
    const std::string& ourDeviceId, bool toDevice,
    const std::string& roomId, const std::string& requestEventId) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto txn = std::make_unique<VerificationTransaction>();
    txn->transactionId = generateTransactionId();
    txn->otherUserId = otherUserId;
    txn->otherDeviceId = otherDeviceId;
    txn->ourDeviceId = ourDeviceId;
    txn->weInitiated = true;
    txn->state = VerificationState::RequestSent;
    txn->roomId = toDevice ? "" : roomId;
    txn->requestEventId = requestEventId;
    txn->startTime = std::chrono::steady_clock::now();
    auto* ptr = txn.get();
    transactions_.push_back(std::move(txn));
    initMasterKeys(ptr);
    return ptr;
}

void VerificationManager::initMasterKeys(VerificationTransaction* txn) {
    if (!txn) return;
    if (ourMasterKeyFn_) txn->ourMasterKey = ourMasterKeyFn_();
    if (theirMasterKeyFn_) txn->theirMasterKey = theirMasterKeyFn_(txn->otherUserId);
}

VerificationTransaction* VerificationManager::findTransaction(const std::string& txnId) {
    std::lock_guard<std::mutex> lk(mtx_);
    // DEBT(AGENTS.md #7): returns a raw pointer used by callers AFTER the lock is
    // released (verify_controller). The sync thread can erase the transaction
    // concurrently (pruning sweep in handleEvent, removeTransaction) -> use-after-free.
    // Real fix belongs with Phase C UI wiring: re-fetch by txnId or hold shared_ptr.
    return findTransactionLocked(txnId);
}

VerificationTransaction* VerificationManager::findTransactionLocked(
    const std::string& txnId) const {
    for (auto& t : transactions_) {
        if (t->transactionId == txnId || t->requestEventId == txnId) return t.get();
    }
    return nullptr;
}

void VerificationManager::removeTransaction(const std::string& txnId) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = transactions_.begin(); it != transactions_.end(); ++it) {
        if ((*it)->transactionId == txnId) {
            transactions_.erase(it);
            return;
        }
    }
}

std::vector<VerificationTransaction*> VerificationManager::activeTransactions() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<VerificationTransaction*> result;
    for (auto& t : transactions_) {
        if (t->state != VerificationState::Done &&
            t->state != VerificationState::Cancelled) {
            result.push_back(t.get());
        }
    }
    return result;
}

static std::string domGetString(simdjson::dom::element e, const std::string& path);

VerificationTransaction* VerificationManager::handleEvent(
    const std::string& eventType, const std::string& senderId,
    const std::string& contentJson, const std::string& ourUserId,
    const std::string& ourDeviceId, const std::string& ourEd25519,
    const std::string& ourCurve25519) {

    std::lock_guard<std::mutex> lk(mtx_);

    // Sweep expired transactions (under held lock — must NOT call removeTransaction).
    for (auto it = transactions_.begin(); it != transactions_.end(); ) {
        if ((*it)->isExpired()) {
            if ((*it)->state != VerificationState::Done &&
                (*it)->state != VerificationState::Cancelled &&
                sendToDeviceFn_) {
                sendToDeviceFn_("m.key.verification.cancel",
                    (*it)->transactionId,
                    buildCancelContent((*it)->transactionId, CancelCode::Timeout),
                    (*it)->otherUserId, (*it)->otherDeviceId);
            }
            it = transactions_.erase(it);
        } else {
            ++it;
        }
    }

    simdjson::dom::parser p;
    auto doc = p.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) return nullptr;

    auto val = doc.value();
    auto txnIdResult = val["transaction_id"].get_string();
    std::string txnId = (txnIdResult.error() == simdjson::SUCCESS)
        ? std::string(txnIdResult.value()) : "";

    auto fromDevice = val["from_device"].get_string();
    std::string otherDeviceId = (fromDevice.error() == simdjson::SUCCESS)
        ? std::string(fromDevice.value()) : "";

    if (txnId.empty()) return nullptr;

    VerificationTransaction* txn = nullptr;

    if (eventType == "m.key.verification.request") {
        auto methodsResult = val["methods"].get_array();
        bool hasSas = false;
        if (methodsResult.error() == simdjson::SUCCESS) {
            for (auto m : methodsResult.value()) {
                auto s = m.get_string();
                if (s.error() == simdjson::SUCCESS && std::string(s.value()) == "m.sas.v1") {
                    hasSas = true; break;
                }
            }
        }
        if (!hasSas) return nullptr;

        auto t = std::make_unique<VerificationTransaction>();
        t->transactionId = txnId;
        t->otherUserId = senderId;
        t->otherDeviceId = otherDeviceId;
        t->ourUserId = ourUserId;
        t->ourDeviceId = ourDeviceId;
        t->ourEd25519 = ourEd25519;
        t->ourCurve25519 = ourCurve25519;
        t->isIncoming = true;
        t->state = VerificationState::RequestReceived;
        t->startTime = std::chrono::steady_clock::now();
        txn = t.get();
        transactions_.push_back(std::move(t));
        initMasterKeys(txn);
        if (stateChangedFn_) stateChangedFn_(txn);
        return txn;
    }

    txn = findTransactionLocked(txnId);
    if (!txn) return nullptr;

    txn->ourUserId = ourUserId;
    txn->ourEd25519 = ourEd25519;
    txn->ourCurve25519 = ourCurve25519;
    if (txn->ourDeviceId.empty()) txn->ourDeviceId = ourDeviceId;

    if (eventType == "m.key.verification.ready") {
        txn->state = VerificationState::Ready;
    } else if (eventType == "m.key.verification.start") {
        txn->state = VerificationState::Started;
        auto method = val["method"].get_string();
        if (method.error() == simdjson::SUCCESS &&
            std::string(method.value()) == "m.sas.v1") {
            txn->sas = sasCreate();
            txn->startContentJson = contentJson;
            std::string commitment = computeCommitment(contentJson, txn->sas.ourPubkey);
            std::string acceptContent = buildAcceptContent(txn->ourDeviceId, txnId, commitment);
            if (sendToDeviceFn_) {
                sendToDeviceFn_("m.key.verification.accept", txnId, acceptContent,
                    txn->otherUserId, txn->otherDeviceId);
                std::string keyContent = buildKeyContent(txn->ourDeviceId,
                    txnId, txn->sas.ourPubkey);
                sendToDeviceFn_("m.key.verification.key", txnId, keyContent,
                    txn->otherUserId, txn->otherDeviceId);
            }
            txn->state = VerificationState::KeySent;
        }
    } else if (eventType == "m.key.verification.accept") {
        txn->state = VerificationState::Accepted;
        auto commitResult = val["commitment"].get_string();
        if (commitResult.error() == simdjson::SUCCESS)
            txn->commitment = std::string(commitResult.value());
    } else if (eventType == "m.key.verification.key") {
        auto keyResult = val["key"].get_string();
        if (keyResult.error() == simdjson::SUCCESS) {
            txn->theirSasPubkey = std::string(keyResult.value());
            if (txn->sas.valid)
                sasSetTheirKey(txn->sas, txn->theirSasPubkey);
            if (txn->theirEd25519.empty() && txn->theirCurve25519.empty() &&
                deviceKeyResolverFn_) {
                if (!deviceKeyResolverFn_(txn->otherUserId, txn->otherDeviceId,
                        txn->theirEd25519, txn->theirCurve25519)) {
                    LOG(LogChannel::E2EE,
                        "handleEvent: device key resolver FAILED for %s/%s — "
                        "their MAC verification will fail (query failed vs real mismatch)",
                        txn->otherUserId.c_str(), txn->otherDeviceId.c_str());
                }
            }
            if (!txn->commitment.empty() && !txn->startContentJson.empty()) {
                std::string expected = computeCommitment(txn->startContentJson,
                    txn->theirSasPubkey);
                if (expected != txn->commitment) {
                    if (sendToDeviceFn_) {
                        sendToDeviceFn_("m.key.verification.cancel", txnId,
                            buildCancelContent(txnId, CancelCode::KeyMismatch),
                            txn->otherUserId, txn->otherDeviceId);
                    }
                    txn->state = VerificationState::Cancelled;
                    txn->cancelCode = CancelCode::KeyMismatch;
                    if (stateChangedFn_) stateChangedFn_(txn);
                    return txn;
                }
            }
            if (txn->state == VerificationState::KeySent)
                txn->state = VerificationState::KeyReceived;
            else if (txn->state != VerificationState::KeyReceived)
                txn->state = VerificationState::KeyReceived;
        }
    } else if (eventType == "m.key.verification.mac") {
        auto cancelMismatch = [&]() {
            if (sendToDeviceFn_) {
                sendToDeviceFn_("m.key.verification.cancel", txnId,
                    buildCancelContent(txnId, CancelCode::KeyMismatch),
                    txn->otherUserId, txn->otherDeviceId);
            }
            txn->state = VerificationState::Cancelled;
            txn->cancelCode = CancelCode::KeyMismatch;
        };
        if (txn->state == VerificationState::MacSent) {
            if (verifyTheirMac(*txn, contentJson)) {
                txn->state = VerificationState::Done;
                if (sendToDeviceFn_) {
                    std::string doneContent = buildDoneContent(txnId);
                    sendToDeviceFn_("m.key.verification.done", txnId, doneContent,
                        txn->otherUserId, txn->otherDeviceId);
                }
            } else {
                cancelMismatch();
            }
        } else {
            if (verifyTheirMac(*txn, contentJson)) {
                txn->state = VerificationState::MacReceived;
            } else {
                cancelMismatch();
            }
        }
    } else if (eventType == "m.key.verification.done") {
        if (txn->state == VerificationState::MacReceived ||
            txn->state == VerificationState::MacSent) {
            txn->state = VerificationState::Done;
        }
    } else if (eventType == "m.key.verification.cancel") {
        txn->state = VerificationState::Cancelled;
        auto codeResult = val["code"].get_string();
        if (codeResult.error() == simdjson::SUCCESS) {
            std::string code(codeResult.value());
            if (code == "m.user") txn->cancelCode = CancelCode::User;
            else if (code == "m.timeout") txn->cancelCode = CancelCode::Timeout;
            else txn->cancelCode = CancelCode::Other;
        }
        if (stateChangedFn_) stateChangedFn_(txn);
    }

    return txn;
}

// ---- Message builders ----

static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) { if (c == '"') out += "\\\""; else out += c; }
    return out;
}

std::string VerificationManager::buildRequestContent(const std::string& ourDeviceId,
    const std::string& txnId) const {
    return "{\"from_device\":\"" + esc(ourDeviceId) + "\","
           "\"transaction_id\":\"" + esc(txnId) + "\","
           "\"methods\":[\"m.sas.v1\"],\"timestamp\":" +
           std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count()) + "}";
}

std::string VerificationManager::buildReadyContent(const std::string& ourDeviceId,
    const std::string& txnId) const {
    return "{\"from_device\":\"" + esc(ourDeviceId) + "\","
           "\"transaction_id\":\"" + esc(txnId) + "\","
           "\"methods\":[\"m.sas.v1\"]}";
}

std::string VerificationManager::buildStartContent(const std::string& ourDeviceId,
    const std::string& txnId) const {
    return "{\"from_device\":\"" + esc(ourDeviceId) + "\","
           "\"transaction_id\":\"" + esc(txnId) + "\","
           "\"method\":\"m.sas.v1\","
           "\"key_agreement_protocols\":[\"curve25519-hkdf-sha256\"],"
           "\"hashes\":[\"sha256\"],"
           "\"message_authentication_codes\":[\"hkdf-hmac-sha256.v2\"],"
           "\"short_authentication_string\":[\"emoji\",\"decimal\"]}";
}

std::string VerificationManager::buildAcceptContent(const std::string& ourDeviceId,
    const std::string& txnId, const std::string& commitment) const {
    return "{\"from_device\":\"" + esc(ourDeviceId) + "\","
           "\"transaction_id\":\"" + esc(txnId) + "\","
           "\"key_agreement_protocol\":\"curve25519-hkdf-sha256\","
           "\"hash\":\"sha256\","
           "\"message_authentication_code\":\"hkdf-hmac-sha256.v2\","
           "\"short_authentication_string\":[\"emoji\",\"decimal\"],"
           "\"commitment\":\"" + esc(commitment) + "\"}";
}

std::string VerificationManager::buildKeyContent(const std::string& ourDeviceId,
    const std::string& txnId, const std::string& sasPubkey) const {
    return "{\"from_device\":\"" + esc(ourDeviceId) + "\","
           "\"transaction_id\":\"" + esc(txnId) + "\","
           "\"key\":\"" + esc(sasPubkey) + "\"}";
}

std::string VerificationManager::buildMacContent(const VerificationTransaction& txn,
    SasSession& sas) const {
    std::string ed25519KeyId = "ed25519:" + txn.ourDeviceId;
    std::string curve25519KeyId = "curve25519:" + txn.ourDeviceId;
    std::string keysSorted = curve25519KeyId + "," + ed25519KeyId;

    // Cross-signing MSK exchange: when BOTH parties have a master key, include
    // the pseudo-device "ed25519:<ourMasterKey>" in the mac (both sides apply
    // the same rule, so the key lists stay symmetric).
    bool useMsk = !txn.ourMasterKey.empty() && !txn.theirMasterKey.empty();
    std::string mskKeyId;
    if (useMsk) {
        // The KEY_IDS mac must be SORTED — the verifier collects the mac-map
        // keys and sorts them; an unsorted append would mismatch whenever the
        // MSK pseudo-device sorts before the device keys.
        mskKeyId = "ed25519:" + txn.ourMasterKey;
        std::vector<std::string> ids = {curve25519KeyId, ed25519KeyId, mskKeyId};
        std::sort(ids.begin(), ids.end());
        keysSorted = ids[0] + "," + ids[1] + "," + ids[2];
    }

    std::string ed25519Info = macInfo(txn.ourUserId, txn.ourDeviceId,
        txn.otherUserId, txn.otherDeviceId, txn.transactionId, ed25519KeyId);
    std::string curve25519Info = macInfo(txn.ourUserId, txn.ourDeviceId,
        txn.otherUserId, txn.otherDeviceId, txn.transactionId, curve25519KeyId);
    std::string keysInfo = macInfo(txn.ourUserId, txn.ourDeviceId,
        txn.otherUserId, txn.otherDeviceId, txn.transactionId, "KEY_IDS");

    std::string ed25519mac = sasCalculateMac(sas, txn.ourEd25519, ed25519Info);
    std::string curve25519mac = sasCalculateMac(sas, txn.ourCurve25519, curve25519Info);
    std::string keysMac = sasCalculateMac(sas, keysSorted, keysInfo);

    std::string macJson = "{\"ed25519:" + esc(txn.ourDeviceId) + "\":\"" + esc(ed25519mac) + "\","
        "\"curve25519:" + esc(txn.ourDeviceId) + "\":\"" + esc(curve25519mac) + "\"";
    if (useMsk) {
        std::string mskInfo = macInfo(txn.ourUserId, txn.ourDeviceId,
            txn.otherUserId, txn.otherDeviceId, txn.transactionId, mskKeyId);
        macJson += ",\"" + esc(mskKeyId) + "\":\"" + esc(sasCalculateMac(sas, txn.ourMasterKey, mskInfo)) + "\"";
    }
    macJson += "}";

    return "{\"from_device\":\"" + esc(txn.ourDeviceId) + "\","
           "\"transaction_id\":\"" + esc(txn.transactionId) + "\","
           "\"mac\":" + macJson + ","
           "\"keys\":\"" + esc(keysMac) + "\"}";
}

std::string VerificationManager::buildDoneContent(const std::string& txnId) const {
    return "{\"transaction_id\":\"" + esc(txnId) + "\"}";
}

std::string VerificationManager::buildCancelContent(const std::string& txnId,
    CancelCode code, const std::string& reason) const {
    return "{\"transaction_id\":\"" + esc(txnId) + "\","
           "\"code\":\"" + cancelCodeToString(code) + "\","
           "\"reason\":\"" + esc(reason.empty() ? cancelCodeToString(code) : reason) + "\"}";
}

std::vector<VerificationEmoji> VerificationManager::computeEmojis(
    VerificationTransaction& txn) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!txn.sas.valid || !txn.sas.theirKeySet) return {};
    std::string info = buildSasInfo(txn);
    std::string bytes = sasGenerateBytes(txn.sas, info);
    if (bytes.empty()) return {};
    return computeSasEmojis(bytes);
}

bool VerificationManager::verifyTheirMac(VerificationTransaction& txn,
    const std::string& theirMacJson) const {
    if (!txn.sas.valid) return false;

    simdjson::dom::parser mp;
    auto md = mp.parse(theirMacJson);
    if (md.error() != simdjson::SUCCESS) return false;

    auto macObjVal = md.value()["mac"];
    auto macObj = macObjVal.get_object();
    auto keysMacResult = md.value()["keys"].get_string();
    if (macObj.error() != simdjson::SUCCESS || keysMacResult.error() != simdjson::SUCCESS)
        return false;

    std::string keyOwnerUser = txn.otherUserId;
    std::string sendingDeviceId = txn.otherDeviceId;
    std::string otherUser = txn.ourUserId;
    std::string receivingDeviceId = txn.ourDeviceId;

    // Collect key IDs from mac object keys, sorted alphabetically
    std::vector<std::string> keyIds;
    for (auto [key, val] : macObj.value()) {
        keyIds.push_back(std::string(key));
        std::string keyId = std::string(key);
        std::string info = macInfo(keyOwnerUser, sendingDeviceId, otherUser,
            receivingDeviceId, txn.transactionId, keyId);
        auto theirMac = val.get_string();
        if (theirMac.error() == simdjson::SUCCESS) {
            std::string keyValue;
            if (!txn.theirMasterKey.empty() && keyId == "ed25519:" + txn.theirMasterKey)
                keyValue = txn.theirMasterKey;  // their MSK pseudo-device
            else if (keyId.find("ed25519:") == 0) keyValue = txn.theirEd25519;
            else if (keyId.find("curve25519:") == 0) keyValue = txn.theirCurve25519;
            if (!sasVerifyMac(txn.sas, std::string(theirMac.value()), keyValue, info))
                return false;
        }
    }

    std::sort(keyIds.begin(), keyIds.end());
    std::string keysSorted;
    for (size_t i = 0; i < keyIds.size(); i++) {
        if (i > 0) keysSorted += ",";
        keysSorted += keyIds[i];
    }

    std::string keysInfo = macInfo(keyOwnerUser, sendingDeviceId, otherUser,
        receivingDeviceId, txn.transactionId, "KEY_IDS");
    std::string theirKeysMac = std::string(keysMacResult.value());
    if (!sasVerifyMac(txn.sas, theirKeysMac, keysSorted, keysInfo))
        return false;

    return true;
}

std::string VerificationManager::buildSasInfo(const VerificationTransaction& txn) const {
    // curve25519-hkdf-sha256 SAS info (7-part pipe-delimited)
    std::string startUser = txn.weInitiated ? txn.otherUserId : txn.ourUserId;
    std::string acceptUser = txn.weInitiated ? txn.ourUserId : txn.otherUserId;
    std::string startDevice = txn.weInitiated ? txn.otherDeviceId : txn.ourDeviceId;
    std::string acceptDevice = txn.weInitiated ? txn.ourDeviceId : txn.otherDeviceId;
    std::string startPubkey = txn.weInitiated ? txn.theirSasPubkey : txn.sas.ourPubkey;
    std::string acceptPubkey = txn.weInitiated ? txn.sas.ourPubkey : txn.theirSasPubkey;

    return "MATRIX_KEY_VERIFICATION_SAS|" + startUser + "|" + startDevice + "|"
           + startPubkey + "|" + acceptUser + "|" + acceptDevice + "|"
           + acceptPubkey + "|" + txn.transactionId;
}

std::string VerificationManager::computeCommitment(const std::string& startContentJson,
    const std::string& ourSasPubkey) const {
    std::string input = startContentJson + ourSasPubkey;
    size_t utilSize = olm_utility_size();
    std::vector<uint8_t> utilMem(utilSize);
    OlmUtility* util = olm_utility(utilMem.data());
    size_t hashLen = olm_sha256_length(util);
    std::vector<uint8_t> hash(hashLen);
    size_t ret = olm_sha256(util, input.data(), input.size(), hash.data(), hashLen);
    if (ret == olm_error()) return {};
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, vb = -6;
    for (size_t i = 0; i < hashLen; i++) {
        val = (val << 8) + hash[i]; vb += 8;
        while (vb >= 0) { out.push_back(b64[(val>>vb)&0x3F]); vb -= 6; }
    }
    if (vb > -6) out.push_back(b64[((val<<8)>>(vb+8))&0x3F]);
    while (out.size()%4) out.push_back('=');
    return out;
}

std::string VerificationManager::macInfo(const std::string& keyOwnerUser,
    const std::string& sendingDeviceId, const std::string& otherUser,
    const std::string& receivingDeviceId,
    const std::string& txnId, const std::string& keyId) const {
    return "MATRIX_KEY_VERIFICATION_MAC" + keyOwnerUser + sendingDeviceId
           + otherUser + receivingDeviceId + txnId + keyId;
}

} // namespace progressive::desktop

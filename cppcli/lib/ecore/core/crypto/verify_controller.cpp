// src/core/crypto/verify_controller.cpp
#include "verify_controller.hpp"
#include "../matrix_client.hpp"
#include "verification.hpp"
#include "../sync_engine.hpp"
#include "../debug_log.hpp"
#include <simdjson.h>

namespace progressive::desktop {

void VerificationController::setVerificationManager(VerificationManager* vm) {
    vm_ = vm;
    if (vm_) {
        vm_->setSendToDeviceFn([this](const std::string& eventType,
            const std::string& txnId, const std::string& contentJson,
            const std::string& targetUserId, const std::string& targetDeviceId) {
            sendToDevice(eventType, txnId, contentJson, targetUserId, targetDeviceId);
        });
        vm_->setDeviceKeyResolverFn([this](const std::string& userId,
            const std::string& deviceId, std::string& outEd25519,
            std::string& outCurve25519) -> bool {
            if (!client_) return false;
            std::string queryBody = "{\"device_keys\":{\"" + userId + "\":[]}}";
            auto resp = client_->queryKeys(queryBody);
            if (!resp.ok) {
                LOG(LogChannel::E2EE,
                    "verifyController: keys/query FAILED http=%d — cannot resolve "
                    "device keys for %s/%s",
                    resp.httpStatus, userId.c_str(), deviceId.c_str());
                return false;
            }
            simdjson::dom::parser p;
            auto doc = p.parse(resp.data);
            if (doc.error() != simdjson::SUCCESS) {
                LOG(LogChannel::E2EE, "verifyController: keys/query response parse failed");
                return false;
            }
            auto userObj = doc.value()["device_keys"][userId];
            if (userObj.error() != simdjson::SUCCESS) {
                LOG(LogChannel::E2EE,
                    "verifyController: user %s not in keys/query response",
                    userId.c_str());
                return false;
            }
            auto devObj = userObj.value()[deviceId];
            if (devObj.error() != simdjson::SUCCESS) {
                LOG(LogChannel::E2EE,
                    "verifyController: device %s/%s not in keys/query response",
                    userId.c_str(), deviceId.c_str());
                return false;
            }
            auto keysObj = devObj.value()["keys"].get_object();
            if (keysObj.error() != simdjson::SUCCESS) return false;
            for (auto k : keysObj.value()) {
                std::string kKey(k.key);
                auto v = k.value.get_string();
                if (v.error() != simdjson::SUCCESS) continue;
                if (kKey == "ed25519:" + deviceId) outEd25519 = std::string(v.value());
                else if (kKey == "curve25519:" + deviceId) outCurve25519 = std::string(v.value());
            }
            if (outEd25519.empty() || outCurve25519.empty()) {
                LOG(LogChannel::E2EE,
                    "verifyController: device %s/%s missing keys (ed=%d curve=%d)",
                    userId.c_str(), deviceId.c_str(),
                    outEd25519.empty() ? 0 : 1, outCurve25519.empty() ? 0 : 1);
                return false;
            }
            return true;
        });
    }
}

void VerificationController::sendToDevice(const std::string& eventType,
    const std::string& txnId, const std::string& contentJson,
    const std::string& targetUserId, const std::string& targetDeviceId) {
    if (!client_) return;
    // Cross-client interop: Element only accepts verification messages that
    // are Olm-encrypted (m.room.encrypted wrapping). Send via the decryptor's
    // Olm path when available; fall back to plain to-device otherwise.
    if (sync_ && sync_->decryptor() && sync_->decryptor()->isInitialized() &&
        !targetUserId.empty() && !targetDeviceId.empty()) {
        bool ok = sync_->decryptor()->sendOlmToDevice(
            targetUserId, targetDeviceId, eventType, contentJson);
        if (ok) {
            LOG(LogChannel::E2EE, "verifyController: sent %s txn=%s to %s/%s (Olm-wrapped)",
                eventType.c_str(), txnId.c_str(), targetUserId.c_str(), targetDeviceId.c_str());
            return;
        }
        LOG(LogChannel::E2EE, "verifyController: Olm send FAILED %s — falling back to plain",
            eventType.c_str());
    }
    std::ostringstream body;
    body << "{\"messages\":{\"" << targetUserId << "\":{\""
         << targetDeviceId << "\":" << contentJson << "}}}";
    client_->sendToDevice(eventType, txnId, body.str());
    LOG(LogChannel::E2EE, "verifyController: sent %s txn=%s to %s/%s",
        eventType.c_str(), txnId.c_str(), targetUserId.c_str(), targetDeviceId.c_str());
}

void VerificationController::startSelfVerification(
    const std::string& ourUserId, const std::string& ourDeviceId,
    const std::string& otherDeviceId) {
    if (!client_ || !vm_) return;
    auto* txn = vm_->startVerification(ourUserId, otherDeviceId, ourDeviceId);
    if (!txn) return;

    std::string content = vm_->buildRequestContent(ourDeviceId, txn->transactionId);
    sendToDevice("m.key.verification.request", txn->transactionId, content,
                  ourUserId, otherDeviceId);
}

void VerificationController::startUserVerification(const std::string& userId,
    const std::string& deviceId) {
    if (!client_ || !vm_) return;
    std::string ourDeviceId = client_->account().deviceId;
    auto* txn = vm_->startVerification(userId, deviceId, ourDeviceId);
    if (!txn) return;

    std::string content = vm_->buildRequestContent(ourDeviceId, txn->transactionId);
    sendToDevice("m.key.verification.request", txn->transactionId, content,
                  userId, deviceId);
}

void VerificationController::acceptIncoming(const std::string& txnId) {
    if (!client_ || !vm_) return;
    auto* txn = vm_->findTransaction(txnId);
    if (!txn) return;

    std::string content = vm_->buildReadyContent(txn->ourDeviceId, txnId);
    sendToDevice("m.key.verification.ready", txnId, content,
                  txn->otherUserId, txn->otherDeviceId);

    content = vm_->buildStartContent(txn->ourDeviceId, txnId);
    txn->startContentJson = content;
    sendToDevice("m.key.verification.start", txnId, content,
                  txn->otherUserId, txn->otherDeviceId);

    // Responder creates SAS and sends key immediately
    txn->sas = sasCreate();
    txn->state = VerificationState::KeySent;

    std::string keyContent = vm_->buildKeyContent(txn->ourDeviceId, txnId,
        txn->sas.ourPubkey);
    sendToDevice("m.key.verification.key", txnId, keyContent,
                  txn->otherUserId, txn->otherDeviceId);
}

void VerificationController::confirmMatch(const std::string& txnId) {
    if (!client_ || !vm_) return;
    auto* txn = vm_->findTransaction(txnId);
    if (!txn || !txn->sas.valid) return;

    std::string macContent = vm_->buildMacContent(*txn, txn->sas);
    sendToDevice("m.key.verification.mac", txnId, macContent,
                  txn->otherUserId, txn->otherDeviceId);

    txn->state = VerificationState::MacSent;
}

void VerificationController::cancelVerification(const std::string& txnId,
    const std::string& reason) {
    cancelVerification(txnId, CancelCode::User, reason);
}

void VerificationController::cancelVerification(const std::string& txnId,
    CancelCode code, const std::string& reason) {
    if (!client_ || !vm_) return;
    auto* txn = vm_->findTransaction(txnId);
    if (!txn) return;

    std::string content = vm_->buildCancelContent(txnId, code, reason);
    sendToDevice("m.key.verification.cancel", txnId, content,
                  txn->otherUserId, txn->otherDeviceId);
    vm_->removeTransaction(txnId);
}

} // namespace progressive::desktop

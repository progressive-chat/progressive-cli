// src/core/crypto/verification.hpp — m.sas.v1 state machine + message builders.
#pragma once
#include "sas.hpp"
#include "sas_emojis.hpp"
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <optional>
#include <mutex>
#include <functional>

namespace progressive::desktop {

enum class VerificationState {
    Idle, RequestSent, RequestReceived, Ready, Started, Accepted,
    KeySent, KeyReceived, MacSent, MacReceived, Done, Cancelled
};

enum class CancelCode {
    User, Timeout, UnknownTransaction, UnknownMethod, UnexpectedMessage,
    KeyMismatch, UserMismatch, InvalidMessage, Accepted, Sasmismatch, Other
};

std::string cancelCodeToString(CancelCode code);

struct VerificationTransaction {
    std::string transactionId;
    std::string otherUserId;
    std::string otherDeviceId;
    std::string ourUserId;
    std::string ourDeviceId;
    std::string ourEd25519;
    std::string ourCurve25519;
    std::string theirEd25519;
    std::string theirCurve25519;
    bool weInitiated = false;
    bool isIncoming = false;
    VerificationState state = VerificationState::Idle;
    SasSession sas;
    std::string theirSasPubkey;
    std::string commitment;
    std::string ourMasterKey;    // our cross-signing master pubkey ("" = not exchanged)
    std::string theirMasterKey;  // the other party's master pubkey ("" = not exchanged)
    std::chrono::steady_clock::time_point startTime;
    std::optional<CancelCode> cancelCode;
    std::string roomId;
    std::string requestEventId;
    std::string startContentJson;
    bool isToDevice() const { return roomId.empty(); }
    bool isExpired() const;
};

class VerificationManager {
public:
    using SendToDeviceFn = std::function<void(const std::string& eventType,
        const std::string& txnId, const std::string& contentJson,
        const std::string& targetUserId, const std::string& targetDeviceId)>;
    using DeviceKeyResolverFn = std::function<bool(
        const std::string& userId, const std::string& deviceId,
        std::string& outEd25519, std::string& outCurve25519)>;
    using StateChangedFn = std::function<void(VerificationTransaction*)>;
    // Our cross-signing master pubkey ("" if we have none).
    using MasterKeyFn = std::function<std::string()>;
    // The other party's master pubkey from /keys/query ("" if they have none).
    using MasterKeyResolverFn = std::function<std::string(const std::string& userId)>;

    void setSendToDeviceFn(SendToDeviceFn fn) { sendToDeviceFn_ = std::move(fn); }
    void setDeviceKeyResolverFn(DeviceKeyResolverFn fn) { deviceKeyResolverFn_ = std::move(fn); }
    void setStateChangedFn(StateChangedFn fn) { stateChangedFn_ = std::move(fn); }
    void setOurMasterKeyFn(MasterKeyFn fn) { ourMasterKeyFn_ = std::move(fn); }
    void setTheirMasterKeyFn(MasterKeyResolverFn fn) { theirMasterKeyFn_ = std::move(fn); }

    static std::string generateTransactionId();

    VerificationTransaction* startVerification(
        const std::string& otherUserId, const std::string& otherDeviceId,
        const std::string& ourDeviceId, bool toDevice = true,
        const std::string& roomId = "", const std::string& requestEventId = "");

    VerificationTransaction* findTransaction(const std::string& txnId);
    void removeTransaction(const std::string& txnId);
    std::vector<VerificationTransaction*> activeTransactions() const;

    VerificationTransaction* handleEvent(const std::string& eventType,
        const std::string& senderId, const std::string& contentJson,
        const std::string& ourUserId, const std::string& ourDeviceId,
        const std::string& ourEd25519,
        const std::string& ourCurve25519);

    std::string buildRequestContent(const std::string& ourDeviceId,
        const std::string& txnId) const;
    std::string buildReadyContent(const std::string& ourDeviceId,
        const std::string& txnId) const;
    std::string buildStartContent(const std::string& ourDeviceId,
        const std::string& txnId) const;
    std::string buildAcceptContent(const std::string& ourDeviceId,
        const std::string& txnId, const std::string& commitment) const;
    std::string buildKeyContent(const std::string& ourDeviceId,
        const std::string& txnId, const std::string& sasPubkey) const;
    std::string buildMacContent(const VerificationTransaction& txn,
        SasSession& sas) const;
    std::string buildDoneContent(const std::string& txnId) const;
    std::string buildCancelContent(const std::string& txnId, CancelCode code,
        const std::string& reason = "") const;

    std::vector<VerificationEmoji> computeEmojis(VerificationTransaction& txn) const;
    bool verifyTheirMac(VerificationTransaction& txn,
        const std::string& theirMacJson) const;
    std::string buildSasInfo(const VerificationTransaction& txn) const;

private:
    mutable std::mutex mtx_;
    std::vector<std::unique_ptr<VerificationTransaction>> transactions_;
    SendToDeviceFn sendToDeviceFn_;
    DeviceKeyResolverFn deviceKeyResolverFn_;
    StateChangedFn stateChangedFn_;
    MasterKeyFn ourMasterKeyFn_;
    MasterKeyResolverFn theirMasterKeyFn_;
    void initMasterKeys(VerificationTransaction* txn);
    VerificationTransaction* findTransactionLocked(const std::string& txnId) const;
    std::string computeCommitment(const std::string& startContentJson,
        const std::string& ourSasPubkey) const;
    std::string macInfo(const std::string& keyOwnerUser,
        const std::string& sendingDeviceId, const std::string& otherUser,
        const std::string& receivingDeviceId,
        const std::string& txnId, const std::string& keyId) const;
};

} // namespace progressive::desktop

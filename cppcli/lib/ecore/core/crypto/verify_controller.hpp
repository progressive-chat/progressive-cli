// src/core/crypto/verify_controller.hpp — SAS send side + UI coordination.
#pragma once
#include <memory>
#include <string>
#include <functional>

namespace progressive::desktop {

class MatrixClient;
class VerificationManager;
class SyncEngine;
enum class CancelCode;

class VerificationController {
public:
    VerificationController() = default;

    void setClient(std::shared_ptr<MatrixClient> c) { client_ = std::move(c); }
    void setVerificationManager(VerificationManager* vm);
    void setSyncEngine(SyncEngine* sync) { sync_ = sync; }

    // Start self-verification (verify our own device against another of our devices)
    void startSelfVerification(const std::string& ourUserId,
                                const std::string& ourDeviceId,
                                const std::string& otherDeviceId);

    // Start verification of another user's device
    void startUserVerification(const std::string& userId,
                               const std::string& deviceId);

    // Accept an incoming verification request
    void acceptIncoming(const std::string& txnId);

    // User confirmed SAS emojis match
    void confirmMatch(const std::string& txnId);

    // User cancelled or emojis don't match
    void cancelVerification(const std::string& txnId, const std::string& reason = "m.user");
    void cancelVerification(const std::string& txnId, CancelCode code,
                            const std::string& reason = "");

    // Send a verification to-device event
    void sendToDevice(const std::string& eventType, const std::string& txnId,
                       const std::string& contentJson,
                       const std::string& targetUserId,
                       const std::string& targetDeviceId);

private:
    std::shared_ptr<MatrixClient> client_;
    VerificationManager* vm_ = nullptr;
    SyncEngine* sync_ = nullptr;
};

} // namespace progressive::desktop

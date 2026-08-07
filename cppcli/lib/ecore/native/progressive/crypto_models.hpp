#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "progressive/event_models.hpp"

namespace progressive {

// ==== Crypto Device Info ====
//
// Original Kotlin (DeviceInfo.kt:26-62):
//   data class DeviceInfo(userId, deviceId, displayName, lastSeenTs, lastSeenIp, ...)

struct DeviceInfo {
    std::string userId;              // "user_id" key
    std::string deviceId;            // "device_id" key
    std::string displayName;         // "display_name" key
    int64_t lastSeenTs = 0;          // "last_seen_ts" key
    std::string lastSeenIp;          // "last_seen_ip" key
    std::string lastSeenUserAgent;
    bool valid = false;   // "last_seen_user_agent" key

    // Original Kotlin: getBestLastSeenUserAgent()
    std::string getBestLastSeenUserAgent() const { return lastSeenUserAgent; }
};

// Original Kotlin (DevicesListResponse.kt:26-29):
//   data class DevicesListResponse(@Json(name="devices") devices: List<DeviceInfo>?)
struct DevicesListResponse {
    std::vector<DeviceInfo> devices;
    int totalCount = 0;
};

// Original Kotlin (UnsignedDeviceInfo.kt:25-31):
//   data class UnsignedDeviceInfo(deviceDisplayName: String?)
struct UnsignedDeviceInfo {
    std::string deviceDisplayName;   // "device_display_name" key
};

// ==== Device Verification ====
//
// Original Kotlin (MXDeviceInfo.kt:169-177):
//   const val DEVICE_VERIFICATION_UNKNOWN = -1
//   const val DEVICE_VERIFICATION_UNVERIFIED = 0
//   const val DEVICE_VERIFICATION_VERIFIED = 1
//   const val DEVICE_VERIFICATION_BLOCKED = 2

enum class DeviceVerification {
    UNKNOWN = -1,
    UNVERIFIED = 0,
    VERIFIED = 1,
    BLOCKED = 2
};

// Original Kotlin (CryptoDeviceInfo.kt:25-53):
//   data class CryptoDeviceInfo(deviceId, userId, algorithms, keys, signatures, unsigned, trustLevel, isBlocked, firstTimeSeenLocalTs)

struct CryptoDeviceInfo {
    std::string deviceId;
    std::string userId;
    std::vector<std::string> algorithms;
    std::unordered_map<std::string, std::string> keys;           // "ed25519:deviceId" → key
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> signatures; // userId → keyType → sig
    UnsignedDeviceInfo unsignedInfo;
    DeviceVerification trustLevel = DeviceVerification::UNKNOWN;
    bool isBlocked = false;
    int64_t firstTimeSeenLocalTs = 0;
    bool valid = false;

    // Original Kotlin: isVerified / isCrossSigningVerified / isUnknown
    bool isVerified() const { return trustLevel == DeviceVerification::VERIFIED; }
    bool isUnknown() const { return trustLevel == DeviceVerification::UNKNOWN; }

    // Original Kotlin: fingerprint()
    std::string fingerprint() const {
        if (deviceId.empty() || keys.empty()) return "";
        auto it = keys.find("ed25519:" + deviceId);
        return it != keys.end() ? it->second : "";
    }

    // Original Kotlin: identityKey()
    std::string identityKey() const {
        if (deviceId.empty() || keys.empty()) return "";
        auto it = keys.find("curve25519:" + deviceId);
        return it != keys.end() ? it->second : "";
    }

    // Original Kotlin: displayName()
    std::string displayName() const { return unsignedInfo.deviceDisplayName; }
};

// ==== Crypto Room Info ====
//
// Original Kotlin (CryptoRoomInfo.kt:24-38):
//   data class CryptoRoomInfo(algorithm, shouldEncryptForInvitedMembers,
//       blacklistUnverifiedDevices, shouldShareHistory, wasEncryptedOnce,
//       rotationPeriodMs, rotationPeriodMsgs)

struct CryptoRoomInfo {
    std::string algorithm;                   // e.g. "m.megolm.v1.aes-sha2"
    bool shouldEncryptForInvitedMembers = false;
    bool blacklistUnverifiedDevices = false;
    bool shouldShareHistory = false;
    bool wasEncryptedOnce = false;
    int64_t rotationPeriodMs = 604800000;     // default: 1 week
    int64_t rotationPeriodMsgs = 100;         // default: 100 messages
};

// ==== Room Encryption Trust Level ====
//
// Original Kotlin (RoomEncryptionTrustLevel.kt:24-38):
//   enum class RoomEncryptionTrustLevel { Default, Warning, Trusted, E2EWithUnsupportedAlgorithm }

enum class RoomEncryptionTrustLevel {
    DEFAULT = 0,           // Black shield — no one verified
    WARNING = 1,           // Red shield — unverified devices
    TRUSTED = 2,           // Green shield — all verified
    E2E_WITH_UNSUPPORTED = 3
};

// ==== User Verification Level ====
//
// Original Kotlin (UserVerificationLevel.kt:21-29):
//   enum class UserVerificationLevel {
//       VERIFIED_ALL_DEVICES_TRUSTED, VERIFIED_WITH_DEVICES_UNTRUSTED,
//       UNVERIFIED_BUT_WAS_PREVIOUSLY, WAS_NEVER_VERIFIED
//   }

enum class UserVerificationLevel {
    VERIFIED_ALL_DEVICES_TRUSTED = 0,
    VERIFIED_WITH_DEVICES_UNTRUSTED = 1,
    UNVERIFIED_BUT_WAS_PREVIOUSLY = 2,
    WAS_NEVER_VERIFIED = 3
};

// ==== Message Verification State ====
//
// Original Kotlin (MXEventDecryptionResult.kt:31-37):
//   enum class MessageVerificationState { VERIFIED, SIGNED_DEVICE_OF_UNVERIFIED_USER,
//       UN_SIGNED_DEVICE_OF_VERIFIED_USER, UN_SIGNED_DEVICE, UNKNOWN_DEVICE, UNSAFE_SOURCE }

enum class MessageVerificationState {
    VERIFIED = 0,
    SIGNED_DEVICE_OF_UNVERIFIED_USER = 1,
    UN_SIGNED_DEVICE_OF_VERIFIED_USER = 2,
    UN_SIGNED_DEVICE = 3,
    UNKNOWN_DEVICE = 4,
    UNSAFE_SOURCE = 5
};

// ==== Forwarded Room Key ====
//
// Original Kotlin (ForwardedRoomKeyContent.kt:27-62):
//   data class ForwardedRoomKeyContent(algorithm, roomId, senderKey, sessionId,
//       sessionKey, forwardingCurve25519KeyChain, senderClaimedEd25519Key, sharedHistory)

struct ForwardedRoomKeyContent {
    std::string algorithm;                 // "algorithm" key
    std::string roomId;                    // "room_id" key
    std::string senderKey;                 // "sender_key" key
    std::string sessionId;                 // "session_id" key
    std::string sessionKey;                // "session_key" key
    std::vector<std::string> forwardingCurve25519KeyChain; // "forwarding_curve25519_key_chain"
    std::string senderClaimedEd25519Key;   // "sender_claimed_ed25519_key"
    bool sharedHistory = false;            // "org.matrix.msc3061.shared_history"
};

// ==== Room Key Share Request ====
//
// Original Kotlin (RoomKeyShareRequest.kt:25-39):
//   data class RoomKeyShareRequest(action, requestingDeviceId, requestId, body)

struct RoomKeyShareRequest {
    std::string action;                    // "action" — "request" or "request_cancellation"
    std::string requestingDeviceId;
    std::string requestId;
    RoomKeyContent body;                   // the requested key info

    bool isShareRequest() const { return action == "request"; }
    bool isCancellation() const { return action == "request_cancellation"; }
};

// ==== Import Room Keys Result ====
//
// Original Kotlin (ImportRoomKeysResult.kt:22-26):
//   data class ImportRoomKeysResult(totalNumberOfKeys, successfullyNumberOfImportedKeys, importedSessionInfo)

struct ImportRoomKeysResult {
    int totalNumberOfKeys = 0;
    int successfullyNumberOfImportedKeys = 0;
    // Map: roomId → senderKey → [sessionId...]
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> importedSessionInfo;
};

// ==== Audit Trail ====
//
// Original Kotlin (AuditTrail.kt)

enum class TrailType {
    OUTGOING_KEY_FORWARD = 0,
    INCOMING_KEY_FORWARD = 1,
    OUTGOING_KEY_WITHHELD = 2,
    INCOMING_KEY_REQUEST = 3,
    UNKNOWN = 4
};

struct AuditInfo {
    std::string roomId;
    std::string sessionId;
    std::string senderKey;
    std::string algorithm;
    std::string userId;
    std::string deviceId;
    int64_t chainIndex = 0;            // ForwardInfo only
    WithHeldCode withheldCode = WithHeldCode::UNAVAILABLE; // WithheldInfo only
    std::string requestId;             // IncomingKeyRequestInfo only
};

struct AuditTrail {
    int64_t ageLocalTs = 0;
    TrailType type = TrailType::UNKNOWN;
    AuditInfo info;
};

// ==== JSON Parsing ====

DeviceInfo parseDeviceInfo(const std::string& json);
DevicesListResponse parseDevicesList(const std::string& json);
CryptoDeviceInfo parseCryptoDeviceInfo(const std::string& json);
ForwardedRoomKeyContent parseForwardedRoomKey(const std::string& json);
RoomKeyShareRequest parseRoomKeyShareRequest(const std::string& json);
ImportRoomKeysResult parseImportRoomKeysResult(const std::string& json);

std::string deviceInfoToJson(const DeviceInfo& info);
std::string cryptoDeviceInfoToJson(const CryptoDeviceInfo& info);

// ==== Global Crypto Config ====
struct GlobalCryptoConfig {
    bool globalBlockUnverifiedDevices = false;
    bool globalEnableKeyGossiping = true;
    bool enableKeyForwardingOnInvite = true;
};

} // namespace progressive

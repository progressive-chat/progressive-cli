#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include "progressive/event_models.hpp"
#include "progressive/crypto_models.hpp"

namespace progressive {

// ==== Login Flow Types ====
//
// Original Kotlin (LoginFlowTypes.kt:21-31):
//   object LoginFlowTypes { const val PASSWORD = "m.login.password", ... }

namespace LoginFlowTypes {
    constexpr const char* PASSWORD = "m.login.password";
    constexpr const char* OAUTH2 = "m.login.oauth2";
    constexpr const char* EMAIL_CODE = "m.login.email.code";
    constexpr const char* EMAIL_URL = "m.login.email.url";
    constexpr const char* EMAIL_IDENTITY = "m.login.email.identity";
    constexpr const char* MSISDN = "m.login.msisdn";
    constexpr const char* RECAPTCHA = "m.login.recaptcha";
    constexpr const char* DUMMY = "m.login.dummy";
    constexpr const char* TERMS = "m.login.terms";
    constexpr const char* TOKEN = "m.login.token";
    constexpr const char* SSO = "m.login.sso";
}

// ==== Well-Known Discovery ====
//
// Original Kotlin (WellKnown.kt:43-70):
//   data class WellKnown(homeServer, identityServer, integrations,
//       unstableDelegatedAuthConfig, disableNetworkConstraint)
//
// JSON: {"m.homeserver":{"base_url":"..."},"m.identity_server":{"base_url":"..."}}

struct WellKnownBaseConfig {
    std::string baseUrl;             // "base_url" key
};

struct DelegatedAuthConfig {
    std::string issuer;              // OIDC issuer URL
    std::string accountUrl;          // account management URL
};

struct WellKnown {
    WellKnownBaseConfig homeServer;           // "m.homeserver" key
    WellKnownBaseConfig identityServer;       // "m.identity_server" key
    std::string integrationsJson;              // "m.integrations" key — raw JSON
    DelegatedAuthConfig delegatedAuth;        // "org.matrix.msc2965.authentication"
    bool disableNetworkConstraint = false;    // "io.element.disable_network_constraint"
};

// ==== Credentials ====
//
// Original Kotlin (Credentials.kt:33-63):
//   data class Credentials(userId, accessToken, refreshToken, homeServer, deviceId, discoveryInformation)
//
// Response from POST /_matrix/client/r0/login

struct DiscoveryInformation {
    WellKnownBaseConfig homeServer;
    WellKnownBaseConfig identityServer;
};

struct Credentials {
    std::string userId;              // "@user:example.org"
    std::string accessToken;         // "syt_..."
    std::string refreshToken;        // optional
    std::string homeServer;          // deprecated, extract from userId
    std::string deviceId;            // "ABCDEFGH"
    DiscoveryInformation discoveryInfo; // well_known from login response

    bool isValid() const {
        return !userId.empty() && !accessToken.empty();
    }
};

// ==== HomeServer Connection Config ====
//
// Original Kotlin (HomeServerConnectionConfig.kt:38-248):
//   data class HomeServerConnectionConfig(homeServerUri, homeServerUriBase,
//       identityServerUri, allowedFingerprints, shouldPin, tlsVersions, etc.)

struct HomeServerConnectionConfig {
    std::string homeServerUrl;           // user-entered homeserver URL
    std::string homeServerUrlBase;       // actual API base URL (may differ after well-known redirect)
    std::string identityServerUrl;       // optional
    std::string antiVirusServerUrl;      // optional
    std::vector<std::string> allowedFingerprints; // TLS certificate fingerprints
    bool shouldPin = false;              // only allow matching fingerprints
    bool shouldAcceptTlsExtensions = true;
    bool allowHttp = false;              // allow non-HTTPS connections
    bool forceUsageTlsVersions = false;
};

// ==== Session Params ====
//
// Original Kotlin (SessionParams.kt:27-80):
//   data class SessionParams(credentials, homeServerConnectionConfig, isTokenValid, loginType)

enum class LoginType {
    UNKNOWN = 0,
    PASSWORD = 1,
    SSO = 2,
    TOKEN = 3
};

struct SessionParams {
    Credentials credentials;
    HomeServerConnectionConfig homeServerConfig;
    bool isTokenValid = true;
    LoginType loginType = LoginType::UNKNOWN;

    // Shortcuts from original Kotlin
    std::string userId() const { return credentials.userId; }
    std::string deviceId() const { return credentials.deviceId; }
    std::string homeServerUrl() const { return homeServerConfig.homeServerUrl; }
    std::string homeServerUrlBase() const { return homeServerConfig.homeServerUrlBase; }
};

// ==== User Presence ====
//
// Original Kotlin (UserPresence.kt:21-24), (PresenceEnum.kt:22-39):
//   data class UserPresence(lastActiveAgo, statusMessage, isCurrentlyActive, presence)

enum class PresenceEnum {
    ONLINE = 0,      // "online"
    OFFLINE = 1,     // "offline"
    UNAVAILABLE = 2, // "unavailable"
    BUSY = 3         // "busy" (MSC3026)
};
const char* presenceEnumToString(PresenceEnum p);
PresenceEnum presenceEnumFromString(const std::string& s);

struct UserPresence {
    int64_t lastActiveAgo = 0;           // milliseconds since last activity
    std::string statusMessage;           // user-set status message
    bool isCurrentlyActive = false;      // user is currently online
    PresenceEnum presence = PresenceEnum::OFFLINE;
};

// ==== Thread Details ====
//
// Original Kotlin (ThreadDetails.kt:26-35), (ThreadNotificationState.kt:24-38):
//   data class ThreadDetails(isRootThread, numberOfThreads, threadSummarySenderInfo,
//       threadSummaryLatestEvent, lastMessageTimestamp, threadNotificationState, isThread, lastRootThreadEdition)

enum class ThreadNotificationState {
    NO_NEW_MESSAGE = 0,
    NEW_MESSAGE = 1,
    NEW_HIGHLIGHTED_MESSAGE = 2
};

struct ThreadDetails {
    bool isRootThread = false;
    int numberOfThreads = 0;
    std::string threadSummarySenderInfoJson; // SenderInfo serialized
    std::string threadSummaryLatestEventJson; // Event serialized
    int64_t lastMessageTimestamp = 0;
    ThreadNotificationState threadNotificationState = ThreadNotificationState::NO_NEW_MESSAGE;
    bool isThread = false;
    std::string lastRootThreadEdition;     // event ID of last root thread edit
};

// ==== JSON Parsing ====

Credentials parseCredentials(const std::string& json);
SessionParams parseSessionParams(const std::string& json);
UserPresence parseUserPresence(const std::string& json);

std::string credentialsToJson(const Credentials& creds);
std::string sessionParamsToJson(const SessionParams& params);
std::string userPresenceToJson(const UserPresence& presence);

// ==== Secure Storage (SSSS) ====
//
// Original Kotlin: securestorage/SecretStorageKeyContent.kt, EncryptedSecretContent.kt,
//   KeyInfoResult.kt, SharedSecretStorageError.kt

struct SsssPassphrase {
    std::string algorithm;       // "m.pbkdf2"
    int iterations = 500000;
    std::string salt;            // base64
};

struct SecretStorageKeyContent {
    std::string algorithm;       // "m.secret_storage.v1.curve25519-aes-sha2"
    std::string name;            // human-readable key name
    SsssPassphrase passphrase;   // for passphrase-based keys
    std::string publicKey;       // base64
};

struct EncryptedSecretContent {
    std::string ciphertext;      // unpadded base64
    std::string mac;             // integrity check
    std::string ephemeral;       // ephemeral key
    std::string iv;              // initialization vector
};

enum class SecureStorageErrorType {
    UNKNOWN_SECRET = 0, UNKNOWN_KEY = 1, UNKNOWN_ALGORITHM = 2,
    UNSUPPORTED_ALGORITHM = 3, SECRET_NOT_ENCRYPTED = 4,
    BAD_KEY_FORMAT = 5, PARSING_ERROR = 6, BAD_MAC = 7, BAD_CIPHERTEXT = 8,
    OTHER = 9
};

struct SecureStorageError {
    SecureStorageErrorType type = SecureStorageErrorType::OTHER;
    std::string detail;          // secret name, key ID, algorithm name, etc.
};

struct KeyInfoResult {
    bool success = false;
    SecretStorageKeyContent content;
    SecureStorageError error;
};

// ==== JSON Parsing ====

SecretStorageKeyContent parseSecretStorageKey(const std::string& json);
EncryptedSecretContent parseEncryptedSecret(const std::string& json);
KeyInfoResult parseKeyInfoResult(const std::string& json);

// ==== Auth Metadata (OAuth 2.0 / MSC4191) ====
//
// Original Kotlin (AuthMetadata.kt:38-48):
//   data class AuthMetadata(issuer, accountManagementUri, accountManagementActionsSupported)

struct AuthMetadata {
    std::string issuer;                                      // OIDC issuer URL
    std::string accountManagementUri;                        // external account management
    std::vector<std::string> accountManagementActions;       // supported actions
};

// ==== Registration Result ====
//
// Original Kotlin (RegistrationResult.kt:28-47):
//   sealed class RegistrationResult { Success(session) / FlowResponse(flowResult) }

struct FlowResult {
    std::vector<std::string> missingStages;
    std::vector<std::string> completedStages;
};

enum class RegistrationResultType { SUCCESS = 0, FLOW_RESPONSE = 1 };

struct RegistrationResult {
    RegistrationResultType type = RegistrationResultType::FLOW_RESPONSE;
    FlowResult flowResult;                                   // for FLOW_RESPONSE
    std::string sessionJson;                                 // for SUCCESS — Session serialized
};

// ==== Well-Known Result (Discovery) ====
//
// Original Kotlin (WellknownResult.kt:24-53):
//   sealed class WellknownResult { Prompt / Ignore / FailPrompt / FailError }

enum class WellknownResultType { PROMPT = 0, IGNORE = 1, FAIL_PROMPT = 2, FAIL_ERROR = 3 };

struct WellknownResult {
    WellknownResultType type = WellknownResultType::IGNORE;
    std::string homeServerUrl;                               // for PROMPT
    std::string identityServerUrl;                           // for PROMPT
    WellKnown wellKnown;                                     // parsed well-known JSON
    std::string errorMessage;                                // for FAIL_ERROR
};

// ==== Room Notification State ====
//
// Original Kotlin (RoomNotificationState.kt:23-30):
//   enum class RoomNotificationState { ALL_MESSAGES_NOISY, ALL_MESSAGES, MENTIONS_ONLY, MUTE }

enum class RoomNotificationState {
    ALL_MESSAGES_NOISY = 0, ALL_MESSAGES = 1, MENTIONS_ONLY = 2, MUTE = 3
};

// ==== Room Sort Order ====
//
// Original Kotlin (RoomSortOrder.kt:22-31):
//   enum class RoomSortOrder { NAME, ACTIVITY, PRIORITY_AND_ACTIVITY, NONE }

enum class RoomSortOrder {
    NAME = 0, ACTIVITY = 1, PRIORITY_AND_ACTIVITY = 2, NONE = 3
};

// ==== Pusher (Push Notifications) ====
//
// Original Kotlin (Pusher.kt:25-56), (PusherData.kt), (PusherState enum)

enum class PusherState { UNREGISTERED = 0, REGISTERING = 1, UNREGISTERING = 2, REGISTERED = 3, FAILED_TO_REGISTER = 4 };

struct PusherData {
    std::string url;                                         // HTTP pusher URL
    std::string format;                                      // "event_id_only"
};

struct Pusher {
    std::string pushKey;
    std::string kind;                                        // "http" or "email"
    std::string appId;
    std::string appDisplayName;
    std::string deviceDisplayName;
    std::string profileTag;
    std::string lang;
    PusherData data;
    bool enabled = true;
    std::string deviceId;
    PusherState state = PusherState::UNREGISTERED;
};

// ==== Content Scanner ====
//
// Original Kotlin (ScanState.kt:21-28): enum + data class ScanStatusInfo

enum class ScanState { TRUSTED = 0, INFECTED = 1, UNKNOWN = 2, IN_PROGRESS = 3 };

struct ScanStatusInfo {
    ScanState state = ScanState::UNKNOWN;
    int64_t scanDateTimestamp = 0;
    std::string humanReadableMessage;
};

// ==== UIA Result ====
//
// Original Kotlin (UiaResult.kt:22-26): enum class UiaResult { SUCCESS, FAILURE, CANCELLED }

enum class UiaResult { SUCCESS = 0, FAILURE = 1, CANCELLED = 2 };

// ==== Preview URL Data ====
//
// Original Kotlin (PreviewUrlData.kt:37-52):
//   data class PreviewUrlData(url, siteName, title, description, mxcUrl, imageWidth, imageHeight)

struct PreviewUrlData {
    std::string url;                                         // og:url
    std::string siteName;                                    // og:site_name
    std::string title;                                       // og:title
    std::string description;                                 // og:description
    std::string mxcUrl;                                      // og:image
    int imageWidth = 0;                                      // og:image:width
    int imageHeight = 0;                                     // og:image:height
};

// ==== Timeline Event ====
//
// Original Kotlin (TimelineEvent.kt:44-71):
//   data class TimelineEvent(root: Event, localId, eventId, displayIndex, senderInfo, annotations, readReceipts)

struct TimelineEvent {
    Event root;
    int64_t localId = 0;
    std::string eventId;
    int displayIndex = 0;
    bool ownedByThreadChunk = false;
    std::string senderInfoJson;                              // SenderInfo serialized
    std::string annotationsJson;                             // EventAnnotationsSummary serialized
    std::vector<std::string> readReceiptIds;                 // read receipt event IDs
    std::string roomId;

    bool isEncrypted() const { return root.type == "m.room.encrypted"; }
};

// ==== JSON Parsing (extended) ====
//
// Original Kotlin (HomeServerCapabilities.kt:25-192):
//   data class HomeServerCapabilities(canChangePassword, canChangeDisplayName,
//       roomVersions, maxUploadFileSize, canUseThreading, ...)

enum class RoomVersionStatus { STABLE = 0, UNSTABLE = 1 };
enum class RoomCapabilitySupport { SUPPORTED = 0, SUPPORTED_UNSTABLE = 1, UNSUPPORTED = 2, UNKNOWN = 3 };

struct RoomVersionInfo {
    std::string version;
    RoomVersionStatus status = RoomVersionStatus::STABLE;
};

struct RoomCapabilityInfo {
    std::string preferred;
    std::vector<std::string> support;
};

struct RoomVersionCapabilities {
    std::string defaultRoomVersion;
    std::vector<RoomVersionInfo> supportedVersion;
    std::unordered_map<std::string, RoomCapabilityInfo> capabilities;
};

struct HomeServerCapabilities {
    bool canChangePassword = true;
    bool canChangeDisplayName = true;
    bool canChangeAvatar = true;
    bool canChange3pid = true;
    int64_t maxUploadFileSize = -1;              // MAX_UPLOAD_FILE_SIZE_UNKNOWN
    bool lastVersionIdentityServerSupported = false;
    std::string defaultIdentityServerUrl;
    RoomVersionCapabilities roomVersions;
    bool canUseThreading = false;
    bool canControlLogoutDevices = false;
    bool canLoginWithQrCode = false;
    bool canUseThreadReadReceipts = false;
    bool canRemotelyTogglePush = false;
    bool canRedactRelatedEvents = false;
    std::string externalAccountManagementUrl;
    std::vector<std::string> externalAccountManagementActions;
    std::string authenticationIssuer;
    bool delegatedOidcAuthEnabled = false;
    bool canUseAuthenticatedMedia = false;

    RoomCapabilitySupport isFeatureSupported(const std::string& feature) const;
    std::string versionOverrideForFeature(const std::string& feature) const;
};

// ==== Identity Service Models ====
//
// Original Kotlin: ThreePid.kt, SharedState.kt, FoundThreePid.kt, IdentityServiceError.kt

enum class ThreePidType { EMAIL = 0, MSISDN = 1 };

struct ThreePid {
    ThreePidType type = ThreePidType::EMAIL;
    std::string value;               // email address or phone number
};

enum class SharedState {
    SHARED = 0, NOT_SHARED = 1, BINDING_IN_PROGRESS = 2
};

struct FoundThreePid {
    ThreePid threePid;
    std::string matrixId;
};

enum class IdentityServiceErrorType {
    OUTDATED_IDENTITY_SERVER = 0, OUTDATED_HOME_SERVER = 1,
    NO_IDENTITY_SERVER_CONFIGURED = 2, TERMS_NOT_SIGNED = 3,
    BULK_LOOKUP_SHA256_NOT_SUPPORTED = 4, USER_CONSENT_NOT_PROVIDED = 5,
    BINDING_ERROR = 6, NO_CURRENT_BINDING = 7
};

// ==== Widget Models ====
//
// Original Kotlin: Widget.kt, WidgetContent.kt, WidgetType.kt

struct WidgetContent {
    std::string creatorUserId;       // "creatorUserId" key
    std::string id;                  // "id" key
    std::string type;                // "type" key — e.g. "m.jitsi"
    std::string url;                 // "url" key
    std::string name;                // "name" key
    std::string dataJson;            // "data" key — raw JSON
    bool waitForIframeLoad = false;  // "waitForIframeLoad" key

    bool isActive() const { return !type.empty() && !url.empty(); }
};

struct Widget {
    WidgetContent widgetContent;
    std::string eventJson;           // Event serialized
    std::string widgetId;
    std::string senderInfoJson;      // SenderInfo serialized
    bool isAddedByMe = false;
    std::string widgetType;          // WidgetType string value
};

// Widget type constants
namespace WidgetTypeConst {
    constexpr const char* JITSI = "m.jitsi";
    constexpr const char* TRADING_VIEW = "m.tradingview";
    constexpr const char* SPOTIFY = "m.spotify";
    constexpr const char* VIDEO = "m.video";
    constexpr const char* GOOGLE_DOC = "m.googledoc";
    constexpr const char* GOOGLE_CALENDAR = "m.googlecalendar";
    constexpr const char* ETHERPAD = "m.etherpad";
    constexpr const char* STICKER_PICKER = "m.stickerpicker";
    constexpr const char* GRAFANA = "m.grafana";
    constexpr const char* CUSTOM = "m.custom";
    constexpr const char* INTEGRATION_MANAGER = "m.integration_manager";
    constexpr const char* ELEMENT_CALL = "io.element.call";
}

// ==== JSON Parsing ====

HomeServerCapabilities parseHomeServerCapabilities(const std::string& json);
WidgetContent parseWidgetContent(const std::string& json);

// ==== Push Rule → Notification State ====
//
// Original Kotlin (RoomPushRuleMapper.kt:69-95):
//   Decision tree analyzing push rule actions → RoomNotificationState

struct PushRuleParams {
    bool enabled = true;
    bool hasNotify = false;
    bool hasSound = false;
    bool isOverride = false;
    std::string roomId;
};

inline RoomNotificationState pushRuleToNotificationState(
    bool enabled, bool hasNotify, bool hasSound, bool isOverride)
{
    if (!enabled) return RoomNotificationState::ALL_MESSAGES;
    if (!hasNotify) {
        return isOverride ? RoomNotificationState::MUTE
                          : RoomNotificationState::MENTIONS_ONLY;
    }
    return hasSound ? RoomNotificationState::ALL_MESSAGES_NOISY
                    : RoomNotificationState::ALL_MESSAGES;
}

inline PushRuleParams notificationStateToPushRule(
    RoomNotificationState state, const std::string& roomId)
{
    PushRuleParams p; p.roomId = roomId;
    if (state == RoomNotificationState::ALL_MESSAGES) { return p; }
    if (state == RoomNotificationState::ALL_MESSAGES_NOISY) {
        p.hasNotify = true; p.hasSound = true; return p;
    }
    p.isOverride = (state == RoomNotificationState::MUTE);
    return p;
}

// ==== Trust Shield ====
//
// Original Kotlin (ComputeShieldForGroupUseCase.kt:31-70):
//   Algorithm: filter trusted → check untrusted devices → green/red/black

inline RoomEncryptionTrustLevel computeShieldForGroup(
    int totalUsers, int trustedUsers, int totalDevices, int untrustedDevices)
{
    if (trustedUsers == 0) return RoomEncryptionTrustLevel::DEFAULT;
    if (untrustedDevices > 0) return RoomEncryptionTrustLevel::WARNING;
    if (totalUsers == trustedUsers) return RoomEncryptionTrustLevel::TRUSTED;
    return RoomEncryptionTrustLevel::DEFAULT;
}

} // namespace progressive

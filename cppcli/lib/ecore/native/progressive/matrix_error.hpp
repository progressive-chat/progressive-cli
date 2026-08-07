#ifndef PROGRESSIVE_MATRIX_ERROR_HPP
#define PROGRESSIVE_MATRIX_ERROR_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace progressive {

// ---- Matrix Error Codes ----
// Faithful port from original Kotlin:
//   org.matrix.android.sdk.api.failure.MatrixError.kt (218 lines)
//
// This is the standard Matrix error response format from the spec:
//   https://matrix.org/docs/spec/client_server/latest#api-standards
//
// Every Matrix API error response has:
//   {"errcode": "M_FORBIDDEN", "error": "You are not allowed to..."}
//
// Some errors include additional fields for specific situations.

// --- Error Code Constants (from MatrixError.kt companion object) ---
namespace ErrorCode {
    constexpr const char* M_FORBIDDEN = "M_FORBIDDEN";
    constexpr const char* M_UNKNOWN = "M_UNKNOWN";
    constexpr const char* M_UNKNOWN_TOKEN = "M_UNKNOWN_TOKEN";
    constexpr const char* M_MISSING_TOKEN = "M_MISSING_TOKEN";
    constexpr const char* M_BAD_JSON = "M_BAD_JSON";
    constexpr const char* M_NOT_JSON = "M_NOT_JSON";
    constexpr const char* M_NOT_FOUND = "M_NOT_FOUND";
    constexpr const char* M_LIMIT_EXCEEDED = "M_LIMIT_EXCEEDED";
    constexpr const char* M_USER_IN_USE = "M_USER_IN_USE";
    constexpr const char* M_ROOM_IN_USE = "M_ROOM_IN_USE";
    constexpr const char* M_BAD_PAGINATION = "M_BAD_PAGINATION";
    constexpr const char* M_UNAUTHORIZED = "M_UNAUTHORIZED";
    constexpr const char* M_OLD_VERSION = "M_OLD_VERSION";
    constexpr const char* M_UNRECOGNIZED = "M_UNRECOGNIZED";
    constexpr const char* M_LOGIN_EMAIL_URL_NOT_YET = "M_LOGIN_EMAIL_URL_NOT_YET";
    constexpr const char* M_THREEPID_AUTH_FAILED = "M_THREEPID_AUTH_FAILED";
    constexpr const char* M_THREEPID_NOT_FOUND = "M_THREEPID_NOT_FOUND";
    constexpr const char* M_THREEPID_IN_USE = "M_THREEPID_IN_USE";
    constexpr const char* M_SERVER_NOT_TRUSTED = "M_SERVER_NOT_TRUSTED";
    constexpr const char* M_TOO_LARGE = "M_TOO_LARGE";
    constexpr const char* M_CONSENT_NOT_GIVEN = "M_CONSENT_NOT_GIVEN";
    constexpr const char* M_RESOURCE_LIMIT_EXCEEDED = "M_RESOURCE_LIMIT_EXCEEDED";
    constexpr const char* M_USER_DEACTIVATED = "M_USER_DEACTIVATED";
    constexpr const char* M_INVALID_USERNAME = "M_INVALID_USERNAME";
    constexpr const char* M_INVALID_ROOM_STATE = "M_INVALID_ROOM_STATE";
    constexpr const char* M_THREEPID_DENIED = "M_THREEPID_DENIED";
    constexpr const char* M_UNSUPPORTED_ROOM_VERSION = "M_UNSUPPORTED_ROOM_VERSION";
    constexpr const char* M_INCOMPATIBLE_ROOM_VERSION = "M_INCOMPATIBLE_ROOM_VERSION";
    constexpr const char* M_BAD_STATE = "M_BAD_STATE";
    constexpr const char* M_GUEST_ACCESS_FORBIDDEN = "M_GUEST_ACCESS_FORBIDDEN";
    constexpr const char* M_CAPTCHA_NEEDED = "M_CAPTCHA_NEEDED";
    constexpr const char* M_CAPTCHA_INVALID = "M_CAPTCHA_INVALID";
    constexpr const char* M_MISSING_PARAM = "M_MISSING_PARAM";
    constexpr const char* M_INVALID_PARAM = "M_INVALID_PARAM";
    constexpr const char* M_EXCLUSIVE = "M_EXCLUSIVE";
    constexpr const char* M_CANNOT_LEAVE_SERVER_NOTICE_ROOM = "M_CANNOT_LEAVE_SERVER_NOTICE_ROOM";
    constexpr const char* M_WRONG_ROOM_KEYS_VERSION = "M_WRONG_ROOM_KEYS_VERSION";
    constexpr const char* M_WEAK_PASSWORD = "M_WEAK_PASSWORD";
    constexpr const char* M_PASSWORD_TOO_SHORT = "M_PASSWORD_TOO_SHORT";
    constexpr const char* M_PASSWORD_NO_DIGIT = "M_PASSWORD_NO_DIGIT";
    constexpr const char* M_PASSWORD_NO_UPPERCASE = "M_PASSWORD_NO_UPPERCASE";
    constexpr const char* M_PASSWORD_NO_LOWERCASE = "M_PASSWORD_NO_LOWERCASE";
    constexpr const char* M_PASSWORD_NO_SYMBOL = "M_PASSWORD_NO_SYMBOL";
    constexpr const char* M_PASSWORD_IN_DICTIONARY = "M_PASSWORD_IN_DICTIONARY";
    constexpr const char* M_TERMS_NOT_SIGNED = "M_TERMS_NOT_SIGNED";
    constexpr const char* M_INVALID_PEPPER = "M_INVALID_PEPPER";
    constexpr const char* ORG_MATRIX_EXPIRED_ACCOUNT = "ORG_MATRIX_EXPIRED_ACCOUNT";
    constexpr const char* LIMIT_TYPE_MAU = "monthly_active_user";
}

// --- Parsed Matrix Error ---
struct MatrixError {
    std::string code;              // M_FORBIDDEN, M_UNKNOWN, etc.
    std::string message;           // human-readable error message

    // Optional fields for specific error types
    std::optional<std::string> consentUri;       // M_CONSENT_NOT_GIVEN
    std::optional<std::string> limitType;        // M_RESOURCE_LIMIT_EXCEEDED
    std::optional<std::string> adminUri;         // M_RESOURCE_LIMIT_EXCEEDED
    std::optional<int64_t> retryAfterMs;         // M_LIMIT_EXCEEDED
    std::optional<bool> isSoftLogout;            // M_UNKNOWN_TOKEN
    std::optional<std::string> newLookupPepper;  // M_INVALID_PEPPER
    std::optional<std::string> session;          // UIA
    bool valid = false;                          // has errcode + error
};

// Parse a Matrix error JSON response.
MatrixError parseMatrixErrorJson(const std::string& json);

// Get a human-readable description for a Matrix error code.
std::string getErrorDescription(const std::string& errorCode);

// Check if a rate limit error (M_LIMIT_EXCEEDED).
bool isRateLimitError(const MatrixError& error);

// Get the retry delay from a rate limit error (ms).
int64_t getRetryAfterMs(const MatrixError& error);

// Check if the error is a soft logout (M_UNKNOWN_TOKEN + soft_logout=true).
bool isSoftLogout(const MatrixError& error);

// Check if consent is needed (M_CONSENT_NOT_GIVEN).
bool needsConsent(const MatrixError& error);

// Check if the error indicates user deactivation.
bool isUserDeactivated(const MatrixError& error);

// Check if the error is a password validation error.
bool isPasswordError(const std::string& errorCode);

// Get a list of all known Matrix error codes.
std::vector<std::string> getAllErrorCodes();

// Format error as JSON for Kotlin UI.
std::string matrixErrorToJson(const MatrixError& error);

// ---- Error Classification (ported from failure/Extensions.kt) ----
// These classify Matrix API errors by HTTP code + error code + message.
// Used for login/auth flow error handling and UI decisions.

struct ErrorContext {
    int httpCode = 0;
    std::string errorCode;
    std::string errorMessage;
    bool isNetworkError = false;
    bool isUnknownHost = false;
    bool isCertificateError = false;
};

// HTTP status code checks
inline bool is400(const ErrorContext& ctx) { return ctx.httpCode == 400; }
inline bool is401(const ErrorContext& ctx) { return ctx.httpCode == 401 && ctx.errorCode == ErrorCode::M_UNAUTHORIZED; }
inline bool is404(const ErrorContext& ctx) { return ctx.httpCode == 404 && ctx.errorCode == ErrorCode::M_NOT_FOUND; }

// Token errors (M_UNKNOWN_TOKEN / M_MISSING_TOKEN / expired account)
inline bool isTokenError(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_UNKNOWN_TOKEN ||
           ctx.errorCode == ErrorCode::M_MISSING_TOKEN ||
           ctx.errorCode == ErrorCode::ORG_MATRIX_EXPIRED_ACCOUNT;
}

// Rate limit exceeded (429 + M_LIMIT_EXCEEDED)
inline bool isLimitExceededError(const ErrorContext& ctx) {
    return ctx.httpCode == 429 && ctx.errorCode == ErrorCode::M_LIMIT_EXCEEDED;
}

// Whether the request should be automatically retried
inline bool shouldBeRetried(const ErrorContext& ctx) {
    return ctx.isNetworkError || isLimitExceededError(ctx);
}

// Retry delay — rate limit retry-after + 100ms buffer, or default
inline int64_t getRetryDelayMs(const ErrorContext& ctx, int64_t retryAfterMs, int64_t defaultMs) {
    if (isLimitExceededError(ctx)) return retryAfterMs + 100;
    return defaultMs;
}

// Username validation errors
inline bool isUsernameInUse(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_USER_IN_USE;
}

inline bool isInvalidUsername(const ErrorContext& ctx) {
    if (ctx.errorCode == ErrorCode::M_INVALID_USERNAME) return true;
    if (ctx.errorCode == ErrorCode::M_UNKNOWN &&
        ctx.errorMessage == "Query parameter 'username' must be ascii") return true;
    return false;
}

// Password errors
inline bool isInvalidPassword(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_FORBIDDEN &&
           ctx.errorMessage == "Invalid password";
}

inline bool isWeakPassword(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_WEAK_PASSWORD;
}

// Registration errors
inline bool isRegistrationDisabled(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_FORBIDDEN && ctx.httpCode == 403;
}

inline bool isRegistrationAvailabilityError(const ErrorContext& ctx) {
    return ctx.httpCode == 400 &&
           (ctx.errorCode == ErrorCode::M_USER_IN_USE ||
            ctx.errorCode == ErrorCode::M_INVALID_USERNAME ||
            ctx.errorCode == ErrorCode::M_EXCLUSIVE);
}

// Login email unknown (M_FORBIDDEN with empty message)
inline bool isLoginEmailUnknown(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_FORBIDDEN && ctx.errorMessage.empty();
}

// User-Interactive Authentication (UIA) — flows present means more auth needed
inline bool isInvalidUIAAuth(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_FORBIDDEN;
    // Note: Kotlin checks error.flows != null; C++ caller must check separately
}

// Homeserver connectivity checks
inline bool isHomeserverUnavailable(const ErrorContext& ctx) {
    return ctx.isNetworkError && ctx.isUnknownHost;
}

inline bool isHomeserverConnectionError(const ErrorContext& ctx) {
    return ctx.isNetworkError;
}

inline bool isUnrecognisedCertificate(const ErrorContext& ctx) {
    return ctx.isCertificateError;
}

// Email verification missing (specific M_UNAUTHORIZED message)
inline bool isMissingEmailVerification(const ErrorContext& ctx) {
    return ctx.errorCode == ErrorCode::M_UNAUTHORIZED &&
           ctx.errorMessage == "Unable to get validated threepid";
}

// ---- Human-Readable Error Classification (ported from ErrorFormatter.kt) ----
// Maps Matrix error context to a specific error type for UI display.

enum class HumanErrorType {
    NONE,
    NETWORK_TIMEOUT,
    SSL_PEER_UNVERIFIED,
    SSL_OTHER,
    NO_NETWORK,
    TERMS_NOT_ACCEPTED,
    INVALID_PASSWORD,
    USER_IN_USE,
    BAD_JSON,
    NOT_JSON,
    THREEPID_DENIED,
    RATE_LIMITED,
    FILE_TOO_BIG,
    THREEPID_NOT_FOUND,
    USER_DEACTIVATED,
    EMAIL_ALREADY_USED,
    PHONE_ALREADY_USED,
    THREEPID_AUTH_FAILED,
    ROOM_ACCESS_UNAUTHORIZED,
    UNVERIFIED_EMAIL,
    HOMESERVER_NOT_FOUND,
    UNAUTHORIZED,
    UNKNOWN_ERROR,
};

inline HumanErrorType classifyError(const ErrorContext& ctx) {
    namespace EC = ErrorCode;
    if (ctx.isNetworkError) {
        if (ctx.isUnknownHost) return HumanErrorType::NO_NETWORK;
        if (ctx.errorCode == "SSL_PEER_UNVERIFIED") return HumanErrorType::SSL_PEER_UNVERIFIED;
        if (ctx.errorCode == "SSL_OTHER") return HumanErrorType::SSL_OTHER;
        return HumanErrorType::NO_NETWORK;
    }
    if (ctx.errorCode == EC::M_CONSENT_NOT_GIVEN)
        return HumanErrorType::TERMS_NOT_ACCEPTED;
    if (isInvalidPassword(ctx))
        return HumanErrorType::INVALID_PASSWORD;
    if (ctx.errorCode == EC::M_USER_IN_USE)
        return HumanErrorType::USER_IN_USE;
    if (ctx.errorCode == EC::M_BAD_JSON)
        return HumanErrorType::BAD_JSON;
    if (ctx.errorCode == EC::M_NOT_JSON)
        return HumanErrorType::NOT_JSON;
    if (ctx.errorCode == EC::M_THREEPID_DENIED)
        return HumanErrorType::THREEPID_DENIED;
    if (isLimitExceededError(ctx))
        return HumanErrorType::RATE_LIMITED;
    if (ctx.errorCode == EC::M_TOO_LARGE)
        return HumanErrorType::FILE_TOO_BIG;
    if (ctx.errorCode == EC::M_THREEPID_NOT_FOUND)
        return HumanErrorType::THREEPID_NOT_FOUND;
    if (ctx.errorCode == EC::M_USER_DEACTIVATED)
        return HumanErrorType::USER_DEACTIVATED;
    if (ctx.errorCode == EC::M_THREEPID_IN_USE) {
        if (ctx.errorMessage == "Email is already in use")
            return HumanErrorType::EMAIL_ALREADY_USED;
        if (ctx.errorMessage == "MSISDN is already in use")
            return HumanErrorType::PHONE_ALREADY_USED;
    }
    if (ctx.errorCode == EC::M_THREEPID_AUTH_FAILED)
        return HumanErrorType::THREEPID_AUTH_FAILED;
    if (ctx.errorCode == EC::M_UNKNOWN &&
        ctx.errorMessage == "Not allowed to join this room")
        return HumanErrorType::ROOM_ACCESS_UNAUTHORIZED;
    if (isMissingEmailVerification(ctx))
        return HumanErrorType::UNVERIFIED_EMAIL;
    if (ctx.httpCode == 404)
        return HumanErrorType::HOMESERVER_NOT_FOUND;
    if (ctx.httpCode == 401)
        return HumanErrorType::UNAUTHORIZED;
    return HumanErrorType::UNKNOWN_ERROR;
}

// Get a stable string identifier for error type (for logging/analytics).
inline const char* humanErrorTypeName(HumanErrorType t) {
    switch (t) {
        case HumanErrorType::NONE: return "none";
        case HumanErrorType::NETWORK_TIMEOUT: return "network_timeout";
        case HumanErrorType::SSL_PEER_UNVERIFIED: return "ssl_peer_unverified";
        case HumanErrorType::SSL_OTHER: return "ssl_other";
        case HumanErrorType::NO_NETWORK: return "no_network";
        case HumanErrorType::TERMS_NOT_ACCEPTED: return "terms_not_accepted";
        case HumanErrorType::INVALID_PASSWORD: return "invalid_password";
        case HumanErrorType::USER_IN_USE: return "user_in_use";
        case HumanErrorType::BAD_JSON: return "bad_json";
        case HumanErrorType::NOT_JSON: return "not_json";
        case HumanErrorType::THREEPID_DENIED: return "threepid_denied";
        case HumanErrorType::RATE_LIMITED: return "rate_limited";
        case HumanErrorType::FILE_TOO_BIG: return "file_too_big";
        case HumanErrorType::THREEPID_NOT_FOUND: return "threepid_not_found";
        case HumanErrorType::USER_DEACTIVATED: return "user_deactivated";
        case HumanErrorType::EMAIL_ALREADY_USED: return "email_already_used";
        case HumanErrorType::PHONE_ALREADY_USED: return "phone_already_used";
        case HumanErrorType::THREEPID_AUTH_FAILED: return "threepid_auth_failed";
        case HumanErrorType::ROOM_ACCESS_UNAUTHORIZED: return "room_access_unauthorized";
        case HumanErrorType::UNVERIFIED_EMAIL: return "unverified_email";
        case HumanErrorType::HOMESERVER_NOT_FOUND: return "homeserver_not_found";
        case HumanErrorType::UNAUTHORIZED: return "unauthorized";
        case HumanErrorType::UNKNOWN_ERROR: return "unknown_error";
    }
    return "unknown";
}

} // namespace progressive

#endif // PROGRESSIVE_MATRIX_ERROR_HPP

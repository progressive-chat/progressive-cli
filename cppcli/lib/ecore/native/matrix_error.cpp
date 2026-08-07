#include "progressive/matrix_error.hpp"
#include "progressive/string_utils.hpp"
#include <sstream>

namespace progressive {

// --- Parse Matrix Error JSON ---
MatrixError parseMatrixErrorJson(const std::string& json) {
    MatrixError error;

    auto extractStr = [&](const std::string& key) -> std::string {
        auto search = "\"" + key + "\":\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) {
            search = "\"" + key + "\": \"";
            pos = json.find(search);
        }
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    };

    error.code = extractStr("errcode");
    error.message = extractStr("error");
    error.valid = !error.code.empty() && !error.message.empty();

    if (auto s = extractStr("consent_uri"); !s.empty()) error.consentUri = s;
    if (auto s = extractStr("limit_type"); !s.empty()) error.limitType = s;
    if (auto s = extractStr("admin_contact"); !s.empty()) error.adminUri = s;
    if (auto s = extractStr("lookup_pepper"); !s.empty()) error.newLookupPepper = s;
    if (auto s = extractStr("session"); !s.empty()) error.session = s;

    // retry_after_ms: number field
    auto retryPos = json.find("\"retry_after_ms\":");
    if (retryPos != std::string::npos) {
        retryPos += 17;
        while (retryPos < json.size() && (json[retryPos] == ' ' || json[retryPos] == '\t')) retryPos++;
        int64_t val = 0;
        while (retryPos < json.size() && json[retryPos] >= '0' && json[retryPos] <= '9') {
            val = val * 10 + (json[retryPos] - '0');
            retryPos++;
        }
        error.retryAfterMs = val;
    }

    // soft_logout: boolean field
    if (json.find("\"soft_logout\": true") != std::string::npos ||
        json.find("\"soft_logout\":true") != std::string::npos) {
        error.isSoftLogout = true;
    }

    return error;
}

// --- Error Descriptions ---
std::string getErrorDescription(const std::string& errorCode) {
    if (errorCode == ErrorCode::M_FORBIDDEN) return "Forbidden access, insufficient permissions";
    if (errorCode == ErrorCode::M_UNKNOWN) return "An unknown error occurred";
    if (errorCode == ErrorCode::M_UNKNOWN_TOKEN) return "Access token not recognised — you may need to log in again";
    if (errorCode == ErrorCode::M_MISSING_TOKEN) return "No access token provided";
    if (errorCode == ErrorCode::M_BAD_JSON) return "Request contained malformed JSON";
    if (errorCode == ErrorCode::M_NOT_JSON) return "Request did not contain valid JSON";
    if (errorCode == ErrorCode::M_NOT_FOUND) return "Resource not found";
    if (errorCode == ErrorCode::M_LIMIT_EXCEEDED) return "Too many requests — wait and try again";
    if (errorCode == ErrorCode::M_USER_IN_USE) return "User ID is already taken";
    if (errorCode == ErrorCode::M_ROOM_IN_USE) return "Room alias is already in use";
    if (errorCode == ErrorCode::M_UNAUTHORIZED) return "Request not authorized — check login credentials";
    if (errorCode == ErrorCode::M_UNSUPPORTED_ROOM_VERSION) return "Room version not supported by this server";
    if (errorCode == ErrorCode::M_INCOMPATIBLE_ROOM_VERSION) return "Room version incompatible — upgrade required";
    if (errorCode == ErrorCode::M_CANNOT_LEAVE_SERVER_NOTICE_ROOM) return "Cannot leave the server notices room";
    if (errorCode == ErrorCode::M_WEAK_PASSWORD) return "Password is too weak";
    if (errorCode == ErrorCode::M_PASSWORD_TOO_SHORT) return "Password is too short";
    if (errorCode == ErrorCode::M_PASSWORD_NO_DIGIT) return "Password must contain at least one digit";
    if (errorCode == ErrorCode::M_PASSWORD_NO_UPPERCASE) return "Password must contain at least one uppercase letter";
    if (errorCode == ErrorCode::M_PASSWORD_NO_LOWERCASE) return "Password must contain at least one lowercase letter";
    if (errorCode == ErrorCode::M_PASSWORD_NO_SYMBOL) return "Password must contain at least one symbol";
    if (errorCode == ErrorCode::M_PASSWORD_IN_DICTIONARY) return "Password found in dictionary — choose another";
    if (errorCode == ErrorCode::M_USER_DEACTIVATED) return "This account has been deactivated";
    if (errorCode == ErrorCode::M_CONSENT_NOT_GIVEN) return "User consent required — please accept the terms";
    if (errorCode == ErrorCode::M_GUEST_ACCESS_FORBIDDEN) return "Guest access is not allowed";
    if (errorCode == ErrorCode::M_TERMS_NOT_SIGNED) return "Terms of service must be accepted";
    if (errorCode == ErrorCode::M_THREEPID_IN_USE) return "Email or phone number already in use";
    if (errorCode == ErrorCode::M_THREEPID_NOT_FOUND) return "Email or phone number not found";
    return "Unknown error: " + errorCode;
}

bool isRateLimitError(const MatrixError& error) {
    return error.code == ErrorCode::M_LIMIT_EXCEEDED;
}

int64_t getRetryAfterMs(const MatrixError& error) {
    return error.retryAfterMs.value_or(0);
}

bool isSoftLogout(const MatrixError& error) {
    return error.code == ErrorCode::M_UNKNOWN_TOKEN && error.isSoftLogout.value_or(false);
}

bool needsConsent(const MatrixError& error) {
    return error.code == ErrorCode::M_CONSENT_NOT_GIVEN || error.code == ErrorCode::M_TERMS_NOT_SIGNED;
}

bool isUserDeactivated(const MatrixError& error) {
    return error.code == ErrorCode::M_USER_DEACTIVATED;
}

bool isPasswordError(const std::string& errorCode) {
    return errorCode == ErrorCode::M_WEAK_PASSWORD ||
           errorCode == ErrorCode::M_PASSWORD_TOO_SHORT ||
           errorCode == ErrorCode::M_PASSWORD_NO_DIGIT ||
           errorCode == ErrorCode::M_PASSWORD_NO_UPPERCASE ||
           errorCode == ErrorCode::M_PASSWORD_NO_LOWERCASE ||
           errorCode == ErrorCode::M_PASSWORD_NO_SYMBOL ||
           errorCode == ErrorCode::M_PASSWORD_IN_DICTIONARY;
}

std::vector<std::string> getAllErrorCodes() {
    return {
        ErrorCode::M_FORBIDDEN, ErrorCode::M_UNKNOWN, ErrorCode::M_UNKNOWN_TOKEN,
        ErrorCode::M_MISSING_TOKEN, ErrorCode::M_BAD_JSON, ErrorCode::M_NOT_JSON,
        ErrorCode::M_NOT_FOUND, ErrorCode::M_LIMIT_EXCEEDED, ErrorCode::M_USER_IN_USE,
        ErrorCode::M_ROOM_IN_USE, ErrorCode::M_UNAUTHORIZED, ErrorCode::M_UNSUPPORTED_ROOM_VERSION,
        ErrorCode::M_INCOMPATIBLE_ROOM_VERSION, ErrorCode::M_WEAK_PASSWORD,
        ErrorCode::M_PASSWORD_TOO_SHORT, ErrorCode::M_PASSWORD_NO_DIGIT,
        ErrorCode::M_PASSWORD_NO_UPPERCASE, ErrorCode::M_PASSWORD_NO_LOWERCASE,
        ErrorCode::M_PASSWORD_NO_SYMBOL, ErrorCode::M_PASSWORD_IN_DICTIONARY,
        ErrorCode::M_USER_DEACTIVATED, ErrorCode::M_CONSENT_NOT_GIVEN,
        ErrorCode::M_GUEST_ACCESS_FORBIDDEN, ErrorCode::M_TERMS_NOT_SIGNED,
        ErrorCode::M_CANNOT_LEAVE_SERVER_NOTICE_ROOM, ErrorCode::M_BAD_STATE,
        ErrorCode::M_CAPTCHA_NEEDED, ErrorCode::M_CAPTCHA_INVALID,
        ErrorCode::M_RESOURCE_LIMIT_EXCEEDED, ErrorCode::ORG_MATRIX_EXPIRED_ACCOUNT
    };
}

std::string matrixErrorToJson(const MatrixError& error) {
    auto esc = [](const std::string& s) -> std::string {
        return escapeJson(s);
    };
    std::ostringstream json;
    json << R"({"valid": )" << (error.valid ? "true" : "false") << ",";
    json << R"("code": ")" << esc(error.code) << R"(",)";
    json << R"("message": ")" << esc(error.message) << R"(",)";
    json << R"("description": ")" << esc(getErrorDescription(error.code)) << R"(",)";
    json << R"("isRateLimit": )" << (isRateLimitError(error) ? "true" : "false") << ",";
    json << R"("isSoftLogout": )" << (isSoftLogout(error) ? "true" : "false") << ",";
    json << R"("isPasswordError": )" << (isPasswordError(error.code) ? "true" : "false") << ",";
    if (error.retryAfterMs.has_value()) json << R"("retryAfterMs": )" << *error.retryAfterMs << ",";
    if (error.consentUri.has_value()) json << R"("consentUri": ")" << esc(*error.consentUri) << R"(",)";
    json << R"("isUserDeactivated": )" << (isUserDeactivated(error) ? "true" : "false") << "}";
    return json.str();
}

} // namespace progressive

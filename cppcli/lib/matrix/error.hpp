#pragma once

#include <string>
#include <stdexcept>
#include <map>

namespace matrixcli { namespace matrix {

struct MatrixError {
    std::string errcode;
    std::string error;
    int retry_after_ms = 0;
    int http_status = 0;

    bool is_rate_limited() const { return http_status == 429; }
    bool is_unauthorized() const { return http_status == 401; }
    bool is_forbidden() const { return http_status == 403; }
    bool is_not_found() const { return http_status == 404; }
    bool is_conflict() const { return http_status == 409; }

    bool is_unknown_token() const { return errcode == "M_UNKNOWN_TOKEN"; }
    bool is_bad_json() const { return errcode == "M_BAD_JSON"; }
    bool is_not_json() const { return errcode == "M_NOT_JSON"; }
    bool is_user_in_use() const { return errcode == "M_USER_IN_USE"; }
    bool is_room_in_use() const { return errcode == "M_ROOM_IN_USE"; }
    bool is_bad_pagination() const { return errcode == "M_BAD_PAGINATION"; }
    bool is_guest_access_forbidden() const { return errcode == "M_GUEST_ACCESS_FORBIDDEN"; }
    bool is_limit_exceeded() const { return errcode == "M_LIMIT_EXCEEDED"; }
    bool is_captcha_needed() const { return errcode == "M_CAPTCHA_NEEDED"; }
    bool is_captcha_invalid() const { return errcode == "M_CAPTCHA_INVALID"; }
    bool is_missing_param() const { return errcode == "M_MISSING_PARAM"; }
    bool is_invalid_param() const { return errcode == "M_INVALID_PARAM"; }
    bool is_session_not_validated() const { return errcode == "M_SESSION_NOT_VALIDATED"; }
    bool is_no_guest_access() const { return errcode == "M_NO_GUEST_ACCESS"; }
    bool is_user_deactivated() const { return errcode == "M_USER_DEACTIVATED"; }
    bool is_password_too_short() const { return errcode == "M_PASSWORD_TOO_SHORT"; }
    bool is_password_no_digit() const { return errcode == "M_PASSWORD_NO_DIGIT"; }
    bool is_password_no_lowercase() const { return errcode == "M_PASSWORD_NO_LOWERCASE"; }
    bool is_password_no_uppercase() const { return errcode == "M_PASSWORD_NO_UPPERCASE"; }
    bool is_password_no_symbol() const { return errcode == "M_PASSWORD_NO_SYMBOL"; }
    bool is_password_in_dictionary() const { return errcode == "M_PASSWORD_IN_DICTIONARY"; }
    bool is_weak_password() const { return errcode == "M_WEAK_PASSWORD"; }
    bool is_invalid_username() const { return errcode == "M_INVALID_USERNAME"; }
    bool is_three_pid_in_use() const { return errcode == "M_THREEPID_IN_USE"; }
    bool is_three_pid_not_found() const { return errcode == "M_THREEPID_NOT_FOUND"; }
    bool is_server_not_trusted() const { return errcode == "M_SERVER_NOT_TRUSTED"; }
    bool is_unsupported_room_version() const { return errcode == "M_UNSUPPORTED_ROOM_VERSION"; }
    bool is_incompatible_room_version() const { return errcode == "M_INCOMPATIBLE_ROOM_VERSION"; }
    bool is_expired_account() const { return errcode == "ORG_MATRIX_EXPIRED_ACCOUNT"; }
    bool is_terms_not_signed() const { return errcode == "M_TERMS_NOT_SIGNED"; }
    bool is_too_large() const { return errcode == "M_TOO_LARGE"; }
    bool is_resource_limit_exceeded() const { return errcode == "M_RESOURCE_LIMIT_EXCEEDED"; }
    bool is_waiting_for_processing() const { return http_status == 202; }

    bool should_retry() const {
        return http_status == 429 || (http_status >= 500 && http_status < 600) ||
               is_resource_limit_exceeded() || is_limit_exceeded();
    }

    std::string what() const {
        return "[" + errcode + "] " + error +
               " (HTTP " + std::to_string(http_status) + ")";
    }
};

class MatrixException : public std::runtime_error {
public:
    MatrixException(const MatrixError& err)
        : std::runtime_error(err.what()), _error(err) {}

    const MatrixError& error() const { return _error; }

private:
    MatrixError _error;
};

MatrixError parseMatrixError(const std::string& response_body, int http_status);

}} // namespace matrixcli::matrix

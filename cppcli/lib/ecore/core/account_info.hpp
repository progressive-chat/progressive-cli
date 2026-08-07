// src/core/account_info.hpp — account/session info struct, shared by MatrixClient and SessionStore.
//
// Extracted to break the circular include between matrix_client.hpp and session_store.hpp.

#pragma once

#include <string>

namespace progressive::desktop {

struct AccountInfo {
    std::string userId;                      // "@user:server"
    std::string deviceId;                    // "DEVID"
    std::string homeserverUrl;               // "https://matrix.org"
    std::string accessToken;
    std::string refreshToken;                // optional

    bool isValid() const {
        return !userId.empty() && !accessToken.empty() && !homeserverUrl.empty();
    }
};

} // namespace progressive::desktop

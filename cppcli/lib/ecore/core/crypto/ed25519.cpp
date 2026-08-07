// src/core/crypto/ed25519.cpp
#include "ed25519.hpp"
#include "../debug_log.hpp"

#include <olm/olm.h>
#include <vector>
#include <string>

namespace progressive::desktop {

bool ed25519Verify(const std::string& pubKeyBase64,
                    const std::string& message,
                    const std::string& signatureBase64) {
    size_t utilSize = olm_utility_size();
    std::vector<uint8_t> utilMem(utilSize);
    OlmUtility* util = olm_utility(utilMem.data());

    std::string sigCopy = signatureBase64;
    size_t result = olm_ed25519_verify(
        util,
        pubKeyBase64.data(), pubKeyBase64.size(),
        message.data(), message.size(),
        sigCopy.data(), sigCopy.size()
    );

    if (result == olm_error()) {
        const char* err = olm_utility_last_error(util);
        LOG(LogChannel::E2EE, "ed25519Verify FAILED: %s", err ? err : "(null)");
        return false;
    }
    return true;
}

} // namespace progressive::desktop

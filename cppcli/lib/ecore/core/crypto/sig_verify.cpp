// src/core/crypto/sig_verify.cpp
#include "sig_verify.hpp"
#include "ed25519.hpp"
#include "../debug_log.hpp"
#include <sstream>

namespace progressive::desktop {

std::string buildDeviceKeysCanonical(const std::string& userId,
                                       const std::string& deviceId,
                                       const std::string& curve25519Key,
                                       const std::string& ed25519Key) {
    std::ostringstream dk;
    dk << "{\"algorithms\":[\"m.olm.v1.curve25519-aes-sha2\",\"m.megolm.v1.aes-sha2\"],"
       << "\"device_id\":\"" << deviceId << "\","
       << "\"keys\":{"
       << "\"curve25519:" << deviceId << "\":\"" << curve25519Key << "\","
       << "\"ed25519:" << deviceId << "\":\"" << ed25519Key << "\""
       << "},"
       << "\"user_id\":\"" << userId << "\""
       << "}";
    return dk.str();
}

bool verifyDeviceKeys(const std::string& userId,
                        const std::string& deviceId,
                        const std::string& curve25519Key,
                        const std::string& ed25519Key,
                        const std::string& signatureBase64) {
    if (signatureBase64.empty() || ed25519Key.empty()) {
        return true;
    }
    std::string canonical = buildDeviceKeysCanonical(userId, deviceId,
                                                      curve25519Key, ed25519Key);
    bool ok = ed25519Verify(ed25519Key, canonical, signatureBase64);
    if (!ok) {
        LOG(LogChannel::E2EE, "verifyDeviceKeys: INVALID for %s/%s",
            userId.c_str(), deviceId.c_str());
    }
    return ok;
}

bool verifyOtk(const std::string& deviceEd25519Key,
                 const std::string& otkCurve25519Key,
                 const std::string& signatureBase64) {
    if (signatureBase64.empty() || deviceEd25519Key.empty()) {
        return true;
    }
    std::string canonical = "{\"key\":\"" + otkCurve25519Key + "\"}";
    bool ok = ed25519Verify(deviceEd25519Key, canonical, signatureBase64);
    if (!ok) {
        LOG(LogChannel::E2EE, "verifyOtk: INVALID");
    }
    return ok;
}

} // namespace progressive::desktop

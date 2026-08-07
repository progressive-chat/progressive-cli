// src/core/crypto/sig_verify.hpp — device key + OTK signature verification.
#pragma once
#include <string>

namespace progressive::desktop {

// Build canonical JSON for device_keys verification (mirrors buildKeysUploadBody).
std::string buildDeviceKeysCanonical(const std::string& userId,
                                       const std::string& deviceId,
                                       const std::string& curve25519Key,
                                       const std::string& ed25519Key);

// Verify a device_keys signature. Returns true if valid.
bool verifyDeviceKeys(const std::string& userId,
                        const std::string& deviceId,
                        const std::string& curve25519Key,
                        const std::string& ed25519Key,
                        const std::string& signatureBase64);

// Verify a signed_curve25519 OTK signature. Returns true if valid.
bool verifyOtk(const std::string& deviceEd25519Key,
                 const std::string& otkCurve25519Key,
                 const std::string& signatureBase64);

} // namespace progressive::desktop

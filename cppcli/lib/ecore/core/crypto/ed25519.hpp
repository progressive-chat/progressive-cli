// src/core/crypto/ed25519.hpp — Ed25519 signature verification via libolm.
#pragma once
#include <string>

namespace progressive::desktop {

// Verify an Ed25519 signature. All inputs are base64-encoded.
//   pubKeyBase64: the signer's Ed25519 public key (43 chars base64)
//   message: the raw message bytes that were signed
//   signatureBase64: the signature to verify (86 chars base64)
// Returns true if the signature is valid, false otherwise.
// Thread-safe: creates a fresh OlmUtility per call (stack-allocated).
bool ed25519Verify(const std::string& pubKeyBase64,
                    const std::string& message,
                    const std::string& signatureBase64);

// Convenience: verify a canonical-JSON message (same as ed25519Verify).
inline bool ed25519VerifyJson(const std::string& pubKeyBase64,
                                const std::string& canonicalJson,
                                const std::string& signatureBase64) {
    return ed25519Verify(pubKeyBase64, canonicalJson, signatureBase64);
}

} // namespace progressive::desktop

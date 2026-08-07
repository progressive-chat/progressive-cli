// src/core/crypto/cross_sign.hpp — cross-signing keys (MSC1756) via libsodium.
#pragma once
#include <string>
#include <vector>

namespace progressive::desktop {

struct CrossSigningKeys {
    std::string masterPub, masterPriv;
    std::string userPub, userPriv;
    std::string selfPub, selfPriv;
};

// Generate MSK/USK/SSK ed25519 keypairs (libsodium crypto_sign_keypair).
// Keys are base64-encoded (64-byte libsodium secret keys, 32-byte publics).
CrossSigningKeys generateCrossSigningKeys();

// Sign a message with a base64 ed25519 private key -> base64 signature.
std::string signEd25519(const std::string& privKeyB64, const std::string& message);

// Verify a base64 ed25519 signature over a message.
bool verifyEd25519(const std::string& pubKeyB64, const std::string& message,
                   const std::string& sigB64);

// Build a CrossSigningKey object (spec /keys/device_signing/upload):
// {"keys":{"ed25519:<pub>":"<pub>"},"signatures":{...},"usage":[...],"user_id":"..."}.
// type: "m.cross_signing.master|self_signing|user_signing" (drives usage).
// signingPubB64/privKeyB64: the key that signs this content (master for
// self/user_signing; empty for master itself). userId: our user id.
std::string buildCrossSigningContent(const std::string& type,
                                     const std::string& pubKeyB64,
                                     const std::string& signingPubB64,
                                     const std::string& signingPrivB64,
                                     const std::string& userId,
                                     const std::string& signerUserId = "");

// Canonical form of the "keys" object (the value of a CrossSigningKey.keys).
std::string crossSigningKeysCanonical(const std::string& pubKeyB64);

// Canonical form of the FULL CrossSigningKey (keys + usage + user_id, sorted
// lexicographically) — the signature message the server verifies.
std::string crossSigningKeyCanonical(const std::string& pubKeyB64,
                                     const std::string& usage,
                                     const std::string& userId);

// ===== Phase 6 trust computation =====

// Trust level for a device.
enum class DeviceTrust { Unverified, Trusted, Verified };

// Result of the trust computation for one device of a user.
struct DeviceTrustResult {
    std::string userId;
    std::string deviceId;
    DeviceTrust trust;
};

// Compute trust for all devices of `userId` from a /keys/query response body:
// - the user's published master/self_signing keys (master_keys/self_signing_keys)
// - each device's SSK signature over its canonical device_keys
// Trusted = valid SSK signature (cross-signed); Unverified otherwise. SAS-verified
// devices (Verified) are NOT computed here — the caller overlays the verified_devices
// table (SAS verification is the stronger level).
// When ourUserId/ourUskPub are given (we have cross-signing), a user whose master
// key carries OUR user-signing signature is one we SAS-verified as an identity —
// ALL their devices are upgraded to Verified.
// Returns empty if the user has no published cross-signing keys.
std::vector<DeviceTrustResult> computeDeviceTrust(const std::string& keysQueryJson,
                                                  const std::string& userId,
                                                  const std::string& ourUserId = "",
                                                  const std::string& ourUskPub = "");

} // namespace progressive::desktop

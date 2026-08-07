// src/core/crypto/cross_sign.cpp — cross-signing key generation + signing.
#include "cross_sign.hpp"
#include "olm_account.hpp"
#include "sig_verify.hpp"

#include <sodium.h>
#include <cstring>
#include <vector>
#include <simdjson.h>

namespace progressive::desktop {

namespace {

// Byte-oriented helpers over the canonical string-based base64 (olm_account).
static std::vector<uint8_t> b64Decode(const std::string& in) {
    auto s = base64Decode(in);
    return std::vector<uint8_t>(s.begin(), s.end());
}
static std::string b64Encode(const uint8_t* d, size_t n) {
    return base64Encode(std::string(reinterpret_cast<const char*>(d), n));
}

bool sodiumInitialized() {
    static const bool ok = (sodium_init() >= 0);
    return ok;
}

} // namespace

CrossSigningKeys generateCrossSigningKeys() {
    CrossSigningKeys keys;
    if (!sodiumInitialized()) return keys;
    auto gen = [](std::string& pub, std::string& priv) {
        uint8_t p[crypto_sign_PUBLICKEYBYTES];
        uint8_t s[crypto_sign_SECRETKEYBYTES];
        if (crypto_sign_keypair(p, s) != 0) return false;
        pub = b64Encode(p, sizeof(p));
        priv = b64Encode(s, sizeof(s));
        return true;
    };
    if (!gen(keys.masterPub, keys.masterPriv)) return {};
    if (!gen(keys.userPub, keys.userPriv)) return {};
    if (!gen(keys.selfPub, keys.selfPriv)) return {};
    return keys;
}

std::string signEd25519(const std::string& privKeyB64, const std::string& message) {
    if (!sodiumInitialized()) return {};
    auto priv = b64Decode(privKeyB64);
    if (priv.size() != crypto_sign_SECRETKEYBYTES) return {};
    uint8_t sig[crypto_sign_BYTES];
    unsigned long long sigLen = 0;
    if (crypto_sign_detached(sig, &sigLen, (const uint8_t*)message.data(),
                             message.size(), priv.data()) != 0)
        return {};
    return b64Encode(sig, sigLen);
}

bool verifyEd25519(const std::string& pubKeyB64, const std::string& message,
                   const std::string& sigB64) {
    if (!sodiumInitialized()) return false;
    auto pub = b64Decode(pubKeyB64);
    auto sig = b64Decode(sigB64);
    if (pub.size() != crypto_sign_PUBLICKEYBYTES || sig.size() != crypto_sign_BYTES)
        return false;
    return crypto_sign_verify_detached(sig.data(), (const uint8_t*)message.data(),
                                       message.size(), pub.data()) == 0;
}

std::string crossSigningKeysCanonical(const std::string& pubKeyB64) {
    return "{\"ed25519:" + pubKeyB64 + "\":\"" + pubKeyB64 + "\"}";
}

std::string crossSigningKeyCanonical(const std::string& pubKeyB64,
                                     const std::string& usage,
                                     const std::string& userId) {
    return "{\"keys\":" + crossSigningKeysCanonical(pubKeyB64)
        + ",\"usage\":[\"" + usage + "\"],\"user_id\":\"" + userId + "\"}";
}

std::string buildCrossSigningContent(const std::string& type,
                                     const std::string& pubKeyB64,
                                     const std::string& signingPubB64,
                                     const std::string& signingPrivB64,
                                     const std::string& userId,
                                     const std::string& signerUserId) {
    std::string usage;
    if (type.find("self_signing") != std::string::npos) usage = "self_signing";
    else if (type.find("user_signing") != std::string::npos) usage = "user_signing";
    else usage = "master";

    std::string keysJson = crossSigningKeysCanonical(pubKeyB64);
    std::string sig;
    if (!signingPrivB64.empty()) {
        // The server verifies the signature over the FULL canonical
        // CrossSigningKey (keys + usage + user_id), not just the keys object.
        std::string signedJson = crossSigningKeyCanonical(pubKeyB64, usage, userId);
        sig = signEd25519(signingPrivB64, signedJson);
    }

    // The signatures map is keyed by the SIGNER's user id — for the setup
    // uploads signer == the key's owner, but for cross-user signing (another
    // user's master key signed by our USK) they differ.
    std::string signer = signerUserId.empty() ? userId : signerUserId;
    std::string out = "{\"keys\":" + keysJson;
    if (!sig.empty()) {
        out += ",\"signatures\":{\"" + signer
            + "\":{\"ed25519:" + signingPubB64 + "\":\"" + sig + "\"}}";
    } else {
        out += ",\"signatures\":{}";
    }
    out += ",\"usage\":[\"" + usage + "\"],\"user_id\":\"" + userId + "\"}";
    return out;
}

// ===== Phase 6 trust computation =====

std::vector<DeviceTrustResult> computeDeviceTrust(const std::string& keysQueryJson,
                                                  const std::string& userId,
                                                  const std::string& ourUserId,
                                                  const std::string& ourUskPub) {
    std::vector<DeviceTrustResult> results;

    simdjson::dom::parser p;
    auto doc = p.parse(keysQueryJson);
    if (doc.error() != simdjson::SUCCESS) return results;

    // Cross-user: a user whose master key carries OUR user-signing signature was
    // SAS-verified by us as an identity — all their devices are Verified.
    bool identityVerified = false;
    if (!ourUserId.empty() && !ourUskPub.empty()) {
        auto masterKeys = doc.value()["master_keys"][userId]["keys"].get_object();
        if (masterKeys.error() == simdjson::SUCCESS) {
            for (auto [k, v] : masterKeys.value()) {
                std::string kStr(k);
                if (kStr.find("ed25519:") != 0) continue;
                auto vs = v.get_string();
                if (vs.error() != simdjson::SUCCESS) continue;
                std::string masterPub = std::string(vs.value());
                auto sig = doc.value()["master_keys"][userId]
                    ["signatures"][ourUserId]["ed25519:" + ourUskPub].get_string();
                if (sig.error() != simdjson::SUCCESS) continue;
                if (verifyEd25519(ourUskPub,
                        crossSigningKeyCanonical(masterPub, "master", userId),
                        std::string(sig.value()))) {
                    identityVerified = true;
                }
            }
        }
    }

    // The user's published self-signing key.
    std::string sskPub;
    {
        auto sskObj = doc.value()["self_signing_keys"][userId]["keys"].get_object();
        if (sskObj.error() == simdjson::SUCCESS) {
            for (auto [k, v] : sskObj.value()) {
                std::string kStr(k);
                if (kStr.find("ed25519:") == 0) {
                    auto vs = v.get_string();
                    if (vs.error() == simdjson::SUCCESS) sskPub = std::string(vs.value());
                }
            }
        }
    }
    if (sskPub.empty()) return results;  // no cross-signing published

    auto devices = doc.value()["device_keys"][userId].get_object();
    if (devices.error() != simdjson::SUCCESS) return results;

    for (auto devField : devices.value()) {
        std::string devId(devField.key);
        std::string curve, ed;
        auto keysObj = devField.value["keys"].get_object();
        if (keysObj.error() == simdjson::SUCCESS) {
            for (auto [k, v] : keysObj.value()) {
                auto vs = v.get_string();
                if (vs.error() != simdjson::SUCCESS) continue;
                std::string kStr(k);
                if (kStr == "curve25519:" + devId) curve = std::string(vs.value());
                else if (kStr == "ed25519:" + devId) ed = std::string(vs.value());
            }
        }
        if (curve.empty() || ed.empty()) continue;

        // The SSK's signature over the canonical device_keys.
        std::string sskSig;
        {
            auto sig = devField.value["signatures"][userId]["ed25519:" + sskPub].get_string();
            if (sig.error() == simdjson::SUCCESS) sskSig = std::string(sig.value());
        }
        DeviceTrustResult r;
        r.userId = userId;
        r.deviceId = devId;
        r.trust = DeviceTrust::Unverified;
        if (identityVerified) {
            r.trust = DeviceTrust::Verified;
        } else if (!sskSig.empty() &&
            verifyEd25519(sskPub, buildDeviceKeysCanonical(userId, devId, curve, ed), sskSig)) {
            r.trust = DeviceTrust::Trusted;
        }
        results.push_back(std::move(r));
    }
    return results;
}

} // namespace progressive::desktop

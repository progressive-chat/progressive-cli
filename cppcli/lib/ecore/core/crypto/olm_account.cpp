// src/core/crypto/olm_account.cpp — OlmAccount lifecycle implementation.
//
// Wraps progressive::OlmAccount which in turn wraps libolm's OlmAccount C API.

#include "olm_account.hpp"

#include <progressive/olm.hpp>
#include <olm/olm.h>

#include <cstring>
#include <cstdio>
#include <random>
#include <vector>

namespace progressive::desktop {

// ---- Base64 helpers ----

namespace {
const char B64_C[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string base64Encode(const std::string& data) {
    std::string out;
    int val = 0, vb = -6;
    for (unsigned char c : data) {
        val = (val << 8) + c;
        vb += 8;
        while (vb >= 0) {
            out.push_back(B64_C[(val >> vb) & 0x3F]);
            vb -= 6;
        }
    }
    if (vb > -6) out.push_back(B64_C[((val << 8) >> (vb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::string base64Decode(const std::string& in) {
    std::string out;
    int val = 0, vb = -8;
    for (char c : in) {
        if (c == '=') break;
        const char* p = std::strchr(B64_C, c);
        if (!p) continue;
        val = (val << 6) + static_cast<int>(p - B64_C);
        vb += 6;
        if (vb >= 0) {
            out.push_back(static_cast<char>((val >> vb) & 0xFF));
            vb -= 8;
        }
    }
    return out;
}

// ---- OlmAccountStore ----

OlmAccountStore::OlmAccountStore() {
    account_ = new progressive::OlmAccount();
}

OlmAccountStore::~OlmAccountStore() {
    delete static_cast<progressive::OlmAccount*>(account_);
}

bool OlmAccountStore::create() {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->create();
    return r.success;
}

bool OlmAccountStore::reset() {
    delete static_cast<progressive::OlmAccount*>(account_);
    // Fresh, UNINITIALIZED libolm memory — the only safe target for
    // olm_unpickle_account (unpickling over an initialized account corrupts
    // the identity — the source of the all-A / cloned-identity bugs).
    account_ = new progressive::OlmAccount();
    uploadedKeyCount_ = 0;
    return true;
}

bool OlmAccountStore::load(const std::string& pickle, const std::string& key) {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->unpickle(key, pickle);
    return r.success;
}

std::string OlmAccountStore::save(const std::string& key) {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->pickle(key);
    return r.success ? r.data : std::string{};
}

OlmIdentityKeys OlmAccountStore::identityKeys() const {
    OlmIdentityKeys keys;
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->identityKeys();
    if (!r.success) return keys;
    // r.data is JSON: {"curve25519":"...","ed25519":"..."}
    auto pos = r.data.find("\"curve25519\":\"");
    if (pos != std::string::npos) {
        pos += 14;
        auto end = r.data.find('"', pos);
        if (end != std::string::npos) keys.curve25519 = r.data.substr(pos, end - pos);
    }
    pos = r.data.find("\"ed25519\":\"");
    if (pos != std::string::npos) {
        pos += 11;
        auto end = r.data.find('"', pos);
        if (end != std::string::npos) keys.ed25519 = r.data.substr(pos, end - pos);
    }
    return keys;
}

std::string OlmAccountStore::curve25519Key() const {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->curve25519Key();
    return r.success ? r.data : std::string{};
}

std::string OlmAccountStore::ed25519Key() const {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->ed25519Key();
    return r.success ? r.data : std::string{};
}

std::string OlmAccountStore::sign(const std::string& message) const {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->sign(message);
    return r.success ? r.data : std::string{};
}

std::string OlmAccountStore::generateOneTimeKeys(int count) {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->generateOneTimeKeys(count);
    if (!r.success) return {};
    return r.data;  // JSON with one-time key keys + values
}

bool OlmAccountStore::generateFallbackKey() {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->generateFallbackKey();
    return r.success;
}

std::string OlmAccountStore::unpublishedFallbackKey() {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    auto r = acc->unpublishedFallbackKey();
    return r.success ? r.data : std::string{};
}

void OlmAccountStore::forgetOldFallbackKey() {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    if (!acc) return;
    acc->forgetOldFallbackKey();
}

void OlmAccountStore::markOneTimeKeysPublished() {
    auto* acc = static_cast<progressive::OlmAccount*>(account_);
    if (!acc) return;
    acc->markKeysAsPublished();
}

} // namespace progressive::desktop

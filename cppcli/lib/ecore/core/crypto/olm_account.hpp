// src/core/crypto/olm_account.hpp — OlmAccount lifecycle + device keys.
//
// Wraps progressive::OlmAccount (libolm) for desktop use:
//   - Create/load account, generate identity keys + one-time keys
//   - Upload device keys via /keys/upload
//   - Sign JSON for device verification
//   - Persist account pickle in SQLite
#pragma once

#include <string>
#include <optional>

namespace progressive::desktop {

struct OlmIdentityKeys {
    std::string curve25519;  // identity key for 1:1 Olm sessions
    std::string ed25519;     // fingerprint key for signing
};

class OlmAccountStore {
public:
    OlmAccountStore();
    ~OlmAccountStore();

    // Create a new account (call once on first login).
    // Returns false if account already exists or creation failed.
    bool create();

    // Load account from pickle string. Returns false on failure.
    bool load(const std::string& pickle, const std::string& key);

    // Regenerate the identity keys: destroy + rebuild the underlying account
    // (olm_create_account requires uninitialized memory — never re-call
    // create() on a live account). Peers' 1:1 sessions become invalid and
    // they re-establish fresh ones.
    bool reset();

    // Save account to pickle string. Returns empty on failure.
    std::string save(const std::string& key);

    // Get identity keys (Curve25519 + Ed25519).
    OlmIdentityKeys identityKeys() const;

    // Get Curve25519 key only.
    std::string curve25519Key() const;

    // Get Ed25519 key only.
    std::string ed25519Key() const;

    // Sign a message with Ed25519 key. Returns base64 signature.
    std::string sign(const std::string& message) const;

    // Generate one-time keys (call before uploading to server).
    // Returns the JSON of one-time keys to upload.
    std::string generateOneTimeKeys(int count);

    // Generate a new fallback key. Returns true on success.
    bool generateFallbackKey();

    // Get the unpublished fallback key as JSON ({"curve25519":{"AAAA":"b64"}}).
    // Returns empty if none exists or it has been published already.
    std::string unpublishedFallbackKey();

    // Mark current one-time keys as published (called after /keys/upload success).
    void forgetOldFallbackKey();
    void markOneTimeKeysPublished();

    bool shared() const { return shared_; }
    void markAsShared() { shared_ = true; }
    void setShared(bool s) { shared_ = s; }

    int uploadedKeyCount() const { return uploadedKeyCount_; }
    void setUploadedKeyCount(int c) { uploadedKeyCount_ = c; }

    bool isValid() const { return account_ != nullptr; }

    // Access the underlying progressive::OlmAccount for OlmSession operations.
    void* rawAccount() { return account_; }

private:
    void* account_ = nullptr;  // progressive::OlmAccount*
    bool shared_ = false;
    int uploadedKeyCount_ = 0;
    friend class Decryptor;
};

// Base64 encode/decode for olm pickles (URL-safe-ish, like libolm uses)
std::string base64Encode(const std::string& data);
std::string base64Decode(const std::string& b64);

} // namespace progressive::desktop

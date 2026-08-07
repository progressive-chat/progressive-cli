// src/core/crypto/recovery_key.hpp — SSSS/key-backup recovery key (Phase 7).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace progressive::desktop {

// Standard base58 (Bitcoin alphabet).
std::string base58Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base58Decode(const std::string& input);

// m.megolm_backup.v1 recovery key: base58(32 bytes entropy + 2 bytes parity).
// The parity byte (repeated twice) is the sum of the entropy bytes mod 256.
std::string generateRecoveryKey();
// Validate a recovery key (base58 decodes, length 34, parity matches).
bool isValidRecoveryKey(const std::string& key);

// The 32-byte entropy from a recovery key (the backup private seed).
std::vector<uint8_t> recoveryKeySeed(const std::string& key);

// Derive the backup keypair (curve25519) from the seed: the seed is treated
// as an ed25519 secret -> crypto_sign_ed25519_sk_to_curve25519 -> the
// curve25519 backup secret + public (auth_data.public_key).
struct BackupKeyPair {
    std::string privateKeyB64;
    std::string publicKeyB64;
};
BackupKeyPair deriveBackupKey(const std::vector<uint8_t>& seedB64);

} // namespace progressive::desktop

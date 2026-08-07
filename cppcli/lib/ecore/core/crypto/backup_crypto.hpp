// src/core/crypto/backup_crypto.hpp — megolm key backup (Phase 7).
// m.megolm_backup.v1.curve25519-aes-sha2 structure + crypto_box_seal
// session_data (self-consistent; the spec-exact KDF interop is a follow-up).
#pragma once
#include <string>

namespace progressive::desktop {

struct BackupVersionInfo {
    std::string version;     // server-assigned version id
    std::string algorithm;   // m.megolm_backup.v1.curve25519-aes-sha2
    std::string publicKey;   // auth_data.public_key (curve25519, base64)
};

// The version-info JSON for POST /room_keys/version:
// {"algorithm":...,"auth_data":{"public_key":...,"signatures":{}}}
std::string buildBackupVersionBody(const BackupVersionInfo& info);

// Encrypt a megolm session export (base64 string) for the backup key ->
// the session_data field: {"ephemeral":<b64>, "ciphertext":<b64>, "mac":<b64>}
// (crypto_box_seal: X25519 + XSalsa20-Poly1305, anonymous).
std::string encryptBackupSessionData(const std::string& megolmExportBase64,
                                     const std::string& backupPublicKeyB64);

// Decrypt session_data with the backup PRIVATE key. Returns the megolm
// export base64, or empty on failure.
std::string decryptBackupSessionData(const std::string& sessionDataJson,
                                     const std::string& backupPrivateKeyB64);

// A full backup-key entry (the /room_keys/keys body item):
// {"first_message_index":0,"forwarded_count":0,"is_verified":false,
//  "session_data":{...}}
// NOTE: the entry can NOT carry extra fields — Synapse strips non-spec keys.
// The sender_key therefore rides INSIDE the encrypted session_data (see
// key_backup.cpp): the plaintext is {"sender_key":...,"export":...}.
std::string buildBackupSessionEntry(const std::string& sessionDataJson,
                                    int firstMessageIndex);

} // namespace progressive::desktop

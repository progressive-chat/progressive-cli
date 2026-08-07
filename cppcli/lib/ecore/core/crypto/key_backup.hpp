// src/core/crypto/key_backup.hpp — backup orchestration (Phase 7).
#pragma once
#include <string>

namespace progressive::desktop {

class MatrixClient;
class Decryptor;
class SessionStore;
struct BackupInfo;

// Create a backup version: generate a recovery key, derive the backup
// keypair, POST /room_keys/version, persist the info in the store.
// Returns the recovery key ("" on failure) — the caller shows it ONCE.
std::string createKeyBackup(MatrixClient& client, SessionStore* store,
                            const std::string& userId);

// Upload all exported megolm sessions (exportAllKeys envelope) encrypted to
// the backup key. Returns false on HTTP failure (0 sessions uploaded is ok).
bool uploadKeyBackup(MatrixClient& client, Decryptor& decryptor,
                     const BackupInfo& info);

// Restore: GET the backed-up sessions, decrypt with the recovery key, import
// into the megolm store (pending events replay). Returns the count imported.
int restoreKeyBackup(MatrixClient& client, Decryptor& decryptor,
                     const BackupInfo& info);

} // namespace progressive::desktop

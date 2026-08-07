// src/core/crypto/key_backup.cpp
#include "key_backup.hpp"

#include "backup_crypto.hpp"
#include "decryptor.hpp"
#include "../matrix_client.hpp"
#include "olm_account.hpp"
#include "recovery_key.hpp"
#include "../session_store.hpp"

#include <simdjson.h>
#include <vector>

namespace progressive::desktop {

std::string createKeyBackup(MatrixClient& client, SessionStore* store,
                            const std::string& userId) {
    std::string recoveryKey = generateRecoveryKey();
    if (recoveryKey.empty()) return "";
    auto pair = deriveBackupKey(recoveryKeySeed(recoveryKey));
    if (pair.publicKeyB64.empty()) return "";

    BackupVersionInfo info;
    info.algorithm = "m.megolm_backup.v1.curve25519-aes-sha2";
    info.publicKey = pair.publicKeyB64;
    auto resp = client.createRoomKeysVersion(buildBackupVersionBody(info));
    if (!resp.ok) return "";

    std::string version;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(resp.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto v = doc.value()["version"].get_string();
            if (v.error() == simdjson::SUCCESS) version = std::string(v.value());
        }
    }
    if (version.empty()) return "";

    if (store) {
        BackupInfo bi;
        bi.version = version;
        bi.recoveryKey = recoveryKey;
        bi.publicKey = pair.publicKeyB64;
        bi.algorithm = info.algorithm;
        store->saveBackupInfo(userId, bi);
    }
    return recoveryKey;
}

bool uploadKeyBackup(MatrixClient& client, Decryptor& decryptor,
                     const BackupInfo& info) {
    if (info.publicKey.empty()) return false;
    std::string all = decryptor.exportAllKeys();
    if (all.empty()) return false;

    simdjson::dom::parser p;
    auto doc = p.parse(all);
    if (doc.error() != simdjson::SUCCESS) return false;
    auto rooms = doc.value()["rooms"].get_object();
    if (rooms.error() != simdjson::SUCCESS) return false;

    // {"rooms":{"<roomId>":{"sessions":{"<sessionId>":<entry>}}}}
    std::ostringstream body;
    body << "{\"rooms\":{";
    bool firstRoom = true;
    for (auto room : rooms.value()) {
        std::string roomId(room.key);
        auto sessions = room.value["sessions"].get_array();
        if (sessions.error() != simdjson::SUCCESS) continue;
        std::string roomJson = "{\"sessions\":{";
        bool firstSess = true;
        for (auto sess : sessions.value()) {
            auto sid = sess["session_id"].get_string();
            auto skey = sess["session_key"].get_string();
            auto sKey = sess["sender_key"].get_string();
            if (sid.error() != simdjson::SUCCESS || skey.error() != simdjson::SUCCESS ||
                sKey.error() != simdjson::SUCCESS) continue;
            // Synapse strips non-spec entry fields — the sender_key rides INSIDE
            // the encrypted payload: {"sender_key":...,"export":<megolm export>}.
            // encryptBackupSessionData expects a BASE64 input (it decodes it) —
            // the plain JSON wrapper must be base64-encoded first.
            std::string payload = "{\"sender_key\":\"" + std::string(sKey.value())
                + "\",\"export\":\"" + std::string(skey.value()) + "\"}";
            std::string sd = encryptBackupSessionData(
                base64Encode(payload), info.publicKey);
            if (sd.empty()) continue;
            if (!firstSess) roomJson += ",";
            firstSess = false;
            roomJson += "\"" + std::string(sid.value()) + "\":"
                + buildBackupSessionEntry(sd, 0);
        }
        if (firstSess) continue;  // no usable sessions in this room
        roomJson += "}}";
        if (!firstRoom) body << ",";
        firstRoom = false;
        body << "\"" << roomId << "\":" << roomJson;
    }
    body << "}}";

    auto resp = client.uploadRoomKeys(body.str(), info.version);
    return resp.ok;
}

int restoreKeyBackup(MatrixClient& client, Decryptor& decryptor,
                     const BackupInfo& info) {
    if (info.publicKey.empty() || info.recoveryKey.empty()) return 0;
    auto pair = deriveBackupKey(recoveryKeySeed(info.recoveryKey));
    if (pair.privateKeyB64.empty()) return 0;

    auto resp = client.getRoomKeys(info.version);
    if (!resp.ok) {
        std::fprintf(stderr, "[key-backup] getRoomKeys FAILED http=%d\n", resp.httpStatus);
        return 0;
    }


    simdjson::dom::parser p;
    auto doc = p.parse(resp.data);
    if (doc.error() != simdjson::SUCCESS) return 0;
    auto rooms = doc.value()["rooms"].get_object();
    if (rooms.error() != simdjson::SUCCESS) return 0;

    int imported = 0;
    for (auto room : rooms.value()) {
        std::string roomId(room.key);
        auto sessions = room.value["sessions"].get_object();
        if (sessions.error() != simdjson::SUCCESS) continue;
        for (auto sess : sessions.value()) {
            auto sdVal = sess.value["session_data"];
            if (sdVal.error() != simdjson::SUCCESS) continue;
            // session_data is a JSON OBJECT (ephemeral/ciphertext/mac) —
            // rebuild the string from its known fields for the decryptor
            // (get_string on the object fails; the local tests never
            // round-trip through a server response — the live test caught it).
            auto ep = sdVal.value()["ephemeral"].get_string();
            auto ct = sdVal.value()["ciphertext"].get_string();
            if (ep.error() != simdjson::SUCCESS || ct.error() != simdjson::SUCCESS) continue;
            std::string sdJson = "{\"ephemeral\":\"" + std::string(ep.value())
                + "\",\"ciphertext\":\"" + std::string(ct.value())
                + "\",\"mac\":\"\"}";
            std::string payload = decryptBackupSessionData(sdJson, pair.privateKeyB64);
            if (payload.empty()) continue;
            // decryptBackupSessionData returns BASE64 — decode to the plaintext
            // wrapper before parsing.
            auto plainBytes = base64Decode(payload);
            std::string plain(plainBytes.begin(), plainBytes.end());
            simdjson::dom::parser wp;
            auto wdoc = wp.parse(plain);
            if (wdoc.error() != simdjson::SUCCESS) continue;
            auto sKey = wdoc.value()["sender_key"].get_string();
            auto exp = wdoc.value()["export"].get_string();
            if (sKey.error() != simdjson::SUCCESS || exp.error() != simdjson::SUCCESS) continue;
            std::string realId = decryptor.importSingleSession(
                roomId, std::string(sKey.value()), std::string(exp.value()));
            if (!realId.empty()) imported++;
        }
    }
    return imported;
}

} // namespace progressive::desktop

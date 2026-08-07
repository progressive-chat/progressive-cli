#include "progressive/megolm_decryptor.hpp"
#include <olm/inbound_group_session.h>
#include <olm/olm.h>
#include <cstring>
#include <sstream>
#include <map>
#include <algorithm>
#include <android/log.h>
#include <cstdio>

#define LOG_TAG "MegolmDecryptor"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace progressive {

// ==== Megolm Session ====

MegolmSession createInboundMegolmSession(const std::string& sessionKeyBase64) {
    MegolmSession result;

    size_t size = olm_inbound_group_session_size();
    if (size == 0) return result;

    void* session = malloc(size);
    if (!session) return result;

    auto* olmSession = olm_inbound_group_session(session);
    size_t ret = olm_init_inbound_group_session(
        olmSession,
        (const uint8_t*)sessionKeyBase64.data(), sessionKeyBase64.size());
    std::fprintf(stderr, "[E2EE] addSession: init=%d err=%s\n",
        ret == olm_error() ? 0 : 1,
        ret == olm_error() ? olm_inbound_group_session_last_error(olmSession) : "(none)");
    if (ret == olm_error()) {
        const char* err = olm_inbound_group_session_last_error(olmSession);
        LOGW("olm_init_inbound_group_session failed: %s", err ? err : "unknown");
        olm_clear_inbound_group_session(olmSession);
        free(session);
        return result;
    }

    // Get session ID (base64-encoded)
    size_t idLen = olm_inbound_group_session_id_length(olmSession);
    std::vector<uint8_t> idBuf(idLen);
    ret = olm_inbound_group_session_id(olmSession, idBuf.data(), idLen);
    if (ret == olm_error()) { free(session); return result; }
    idBuf.resize(ret);
    std::string sessionId(idBuf.begin(), idBuf.end());

    result.session = session;
    result.sessionId = sessionId;
    result.firstKnownIndex = (uint32_t)olm_inbound_group_session_first_known_index(olmSession);
    result.valid = true;

    return result;
}

void destroyMegolmSession(MegolmSession& session) {
    if (session.session) {
        olm_clear_inbound_group_session(static_cast<OlmInboundGroupSession*>(session.session));
        free(session.session);
        session.session = nullptr;
    }
    session.valid = false;
}

std::string megolmDecrypt(MegolmSession& session, const std::string& ciphertext) {
    if (!session.valid || !session.session) return "";

    auto* olmSession = static_cast<OlmInboundGroupSession*>(session.session);

    std::vector<uint8_t> msg(ciphertext.begin(), ciphertext.end());

    size_t maxLen = olm_group_decrypt_max_plaintext_length(olmSession, msg.data(), msg.size());
    if (maxLen == olm_error()) return "";

    // libolm clobbers msg in-place during max_plaintext_length; restore before decrypt
    std::memcpy(msg.data(), ciphertext.data(), ciphertext.size());

    std::vector<uint8_t> plaintext(maxLen);
    uint32_t messageIndex = 0;
    size_t ret = olm_group_decrypt(olmSession, msg.data(), msg.size(),
        plaintext.data(), maxLen, &messageIndex);
    std::fprintf(stderr, "[E2EE] megolmDecrypt: ret=%zu err=%s\n",
        ret,
        ret == olm_error() ? olm_inbound_group_session_last_error(olmSession) : "(ok)");
    if (ret == olm_error()) {
        const char* err = olm_inbound_group_session_last_error(olmSession);
        LOGW("olm_group_decrypt failed: %s", err ? err : "unknown");
        return "";
    }

    return std::string(plaintext.begin(), plaintext.begin() + ret);
}

std::string getMegolmSessionId(const MegolmSession& session) {
    return session.sessionId;
}

std::string exportMegolmSession(const MegolmSession& session) {
    if (!session.valid || !session.session) return "";

    auto* olmSession = static_cast<OlmInboundGroupSession*>(session.session);
    size_t len = olm_export_inbound_group_session_length(olmSession);
    if (len == olm_error()) return "";

    std::vector<uint8_t> key(len);
    uint32_t msgIndex = session.firstKnownIndex;
    size_t ret = olm_export_inbound_group_session(olmSession, key.data(), len, msgIndex);
    if (ret == olm_error()) return "";
    return std::string(key.begin(), key.begin() + ret);
}

// ==== Session Manager ====

bool MegolmSessionManager::addSession(const std::string& roomId, const std::string& senderKey,
                                       const std::string& sessionId, const std::string& sessionKeyBase64) {
    auto session = createInboundMegolmSession(sessionKeyBase64);
    if (!session.valid) return false;
    session.senderKey = senderKey;

    SessionKey key{roomId, senderKey, sessionId};
    sessions_[key] = std::move(session);
    return true;
}

std::string MegolmSessionManager::addImportedSession(const std::string& roomId,
    const std::string& senderKey, const std::string& sessionKeyExportBase64) {
    // v1 export format: olm_import_inbound_group_session expects the base64
    // export string (AGENTS.md quirk #4 — never pre-decode).
    size_t size = olm_inbound_group_session_size();
    void* mem = malloc(size);
    if (!mem) return {};
    auto* sess = olm_inbound_group_session(mem);
    size_t ret = olm_import_inbound_group_session(sess,
        (const uint8_t*)sessionKeyExportBase64.data(), sessionKeyExportBase64.size());
    if (ret == (size_t)-1) {
        free(mem);
        return {};
    }
    // Read the real session id (the forwarded content's session_id may differ).
    size_t idLen = olm_inbound_group_session_id_length(sess);
    std::string realId(idLen, '\0');
    if (olm_inbound_group_session_id(sess, (uint8_t*)&realId[0], idLen) == (size_t)-1) {
        free(mem);
        return {};
    }
    MegolmSession session;
    session.valid = true;
    session.session = mem;
    session.sessionId = realId;
    session.senderKey = senderKey;
    session.firstKnownIndex = 0;
    sessions_[SessionKey{roomId, senderKey, realId}] = std::move(session);
    return realId;
}

std::string MegolmSessionManager::exportAllSessionsJson() {
    std::map<std::string, std::vector<const MegolmSession*>> byRoom;
    for (auto& [key, sess] : sessions_) byRoom[key.roomId].push_back(&sess);
    std::ostringstream out;
    out << "{\"version\":1,\"rooms\":{";
    bool firstRoom = true;
    for (auto& [room, list] : byRoom) {
        if (!firstRoom) out << ",";
        firstRoom = false;
        out << "\"" << room << "\":{\"sessions\":[";
        bool firstSess = true;
        for (auto* s : list) {
            std::string key = exportMegolmSession(*s);
            if (key.empty()) continue;
            if (!firstSess) out << ",";
            firstSess = false;
            out << "{\"algorithm\":\"m.megolm.v1.aes-sha2\","
                << "\"room_id\":\"" << room << "\","
                << "\"session_id\":\"" << s->sessionId << "\","
                << "\"session_key\":\"" << key << "\","
                << "\"sender_key\":\"" << s->senderKey << "\","
                << "\"forwarding_curve25519_key_chain\":[]}";
        }
        out << "]}";
    }
    out << "}}";
    return out.str();
}

MegolmSession* MegolmSessionManager::findSession(const std::string& roomId, const std::string& senderKey,
                                                   const std::string& sessionId) {
    SessionKey key{roomId, senderKey, sessionId};
    auto it = sessions_.find(key);
    return it != sessions_.end() ? &it->second : nullptr;
}

void MegolmSessionManager::clearRoom(const std::string& roomId) {
    auto it = sessions_.begin();
    while (it != sessions_.end()) {
        if (it->first.roomId == roomId) {
            destroyMegolmSession(it->second);
            it = sessions_.erase(it);
        } else { ++it; }
    }
}

void MegolmSessionManager::clearAll() {
    for (auto& pair : sessions_) destroyMegolmSession(pair.second);
    sessions_.clear();
}

} // namespace progressive

// src/core/crypto/megolm_store.cpp — Inbound Megolm session storage.

#include "megolm_store.hpp"

#include "core/debug_log.hpp"

#include <progressive/megolm_decryptor.hpp>
#include <progressive/olm.hpp>

#include <algorithm>
#include <sstream>
#include <simdjson.h>

namespace progressive::desktop {

struct SessionParams {
    std::string roomId;
    std::string senderKey;
    std::string sessionId;
    std::string sessionKeyBase64;
};

struct MegolmStore::Impl {
    progressive::MegolmSessionManager mgr;
    std::vector<SessionParams> params;  // for persistence
    bool backupDirty = false;
};

MegolmStore::MegolmStore() : impl_(std::make_unique<Impl>()) {}
MegolmStore::~MegolmStore() = default;

bool MegolmStore::addInboundSession(const std::string& roomId,
                                       const std::string& senderKey,
                                       const std::string& sessionId,
                                       const std::string& sessionKeyBase64) {
    std::lock_guard<std::mutex> lk(mtx_);
    bool ok = impl_->mgr.addSession(roomId, senderKey, sessionId, sessionKeyBase64);
    if (ok) { impl_->params.push_back({roomId, senderKey, sessionId, sessionKeyBase64});
              impl_->backupDirty = true; }
    return ok;
}

std::string MegolmStore::decrypt(const std::string& roomId,
                                    const std::string& senderKey,
                                    const std::string& sessionId,
                                    const std::string& ciphertext) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto* sess = impl_->mgr.findSession(roomId, senderKey, sessionId);
    if (!sess) return {};
    return progressive::megolmDecrypt(*sess, ciphertext);
}

std::string MegolmStore::addImportedSession(const std::string& roomId,
    const std::string& senderKey, const std::string& sessionKeyExportBase64) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string id = impl_->mgr.addImportedSession(roomId, senderKey, sessionKeyExportBase64);
    if (!id.empty()) impl_->backupDirty = true;
    return id;
}

bool MegolmStore::backupDirty() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return impl_->backupDirty;
}

void MegolmStore::markBackupClean() {
    std::lock_guard<std::mutex> lk(mtx_);
    impl_->backupDirty = false;
}

void MegolmStore::markBackupDirty() {
    std::lock_guard<std::mutex> lk(mtx_);
    impl_->backupDirty = true;
}

std::string MegolmStore::exportAllJson() {
    std::lock_guard<std::mutex> lk(mtx_);
    return impl_->mgr.exportAllSessionsJson();
}

std::string MegolmStore::exportSessionKey(const std::string& roomId,
    const std::string& senderKey, const std::string& sessionId) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto* sess = impl_->mgr.findSession(roomId, senderKey, sessionId);
    if (!sess) return {};
    return progressive::exportMegolmSession(*sess);
}

bool MegolmStore::hasSession(const std::string& roomId,
                                const std::string& senderKey,
                                const std::string& sessionId) {
    std::lock_guard<std::mutex> lk(mtx_);
    return impl_->mgr.findSession(roomId, senderKey, sessionId) != nullptr;
}

void MegolmStore::addPending(const PendingEncryptedEvent& evt) {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.push_back(evt);
}

std::vector<PendingEncryptedEvent> MegolmStore::takePendingForSession(
    const std::string& roomId,
    const std::string& senderKey,
    const std::string& sessionId) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<PendingEncryptedEvent> result;
    auto it = std::remove_if(pending_.begin(), pending_.end(),
        [&](const PendingEncryptedEvent& e) {
            if (e.roomId == roomId && e.senderKey == senderKey && e.sessionId == sessionId) {
                result.push_back(e);
                return true;
            }
            return false;
        });
    pending_.erase(it, pending_.end());
    return result;
}

std::string MegolmStore::pickleAll(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (impl_->params.empty()) return "[]";
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < impl_->params.size(); ++i) {
        if (i > 0) os << ",";
        os << "{\"r\":\"" << impl_->params[i].roomId << "\""
           << ",\"k\":\"" << impl_->params[i].senderKey << "\""
           << ",\"s\":\"" << impl_->params[i].sessionId << "\""
           << ",\"d\":\"" << impl_->params[i].sessionKeyBase64 << "\"}";
    }
    os << "]";
    std::string raw = os.str();
    // XOR-encrypt with pickleKey (simple obfuscation on disk)
    if (key.empty()) return raw;
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] ^= key[i % key.size()];
    // Hex-encode for safe storage
    std::string hex;
    for (unsigned char c : raw) {
        static const char h[] = "0123456789abcdef";
        hex += h[c >> 4];
        hex += h[c & 15];
    }
    return hex;
}

bool MegolmStore::unpickleAll(const std::string& key, const std::string& data) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Never mix another account's sessions in: a fresh load starts empty
    // (the manager holds whatever the previous account left behind).
    impl_->mgr.clearAll();
    pending_.clear();
    if (data.empty() || data == "[]") return true;
    // Hex-decode
    std::string raw;
    if (data.size() % 2 == 0) {
        for (size_t i = 0; i < data.size(); i += 2)
            raw += (char)strtol(data.substr(i, 2).c_str(), nullptr, 16);
    } else {
        raw = data;
    }
    // XOR-decrypt
    if (!key.empty()) {
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] ^= key[i % key.size()];
    }
    // Parse JSON array with simdjson (fixes bug #11: manual brace-matching
    // parser used find("}}") which never matched single-} object endings,
    // causing only the first session to be loaded)
    simdjson::dom::parser parser;
    auto root = parser.parse(raw);
    if (root.error() != simdjson::SUCCESS) {
        LOG(LogChannel::E2EE, "unpickleAll: JSON parse FAILED dataLen=%zu rawLen=%zu",
            data.size(), raw.size());
        return false;
    }
    auto arr = root.value().get_array();
    if (arr.error() != simdjson::SUCCESS) {
        LOG(LogChannel::E2EE, "unpickleAll: not an array");
        return false;
    }
    for (auto elem : arr.value()) {
        auto obj = elem.get_object();
        if (obj.error() != simdjson::SUCCESS) continue;
        auto r = obj.value()["r"].get_string();
        auto k = obj.value()["k"].get_string();
        auto s = obj.value()["s"].get_string();
        auto d = obj.value()["d"].get_string();
        if (r.error() != simdjson::SUCCESS || k.error() != simdjson::SUCCESS ||
            s.error() != simdjson::SUCCESS || d.error() != simdjson::SUCCESS) continue;
        std::string roomId(r.value());
        std::string senderKey(k.value());
        std::string sessionId(s.value());
        std::string sessionKey(d.value());
        // Dedup: skip if session already exists
        if (!impl_->mgr.findSession(roomId, senderKey, sessionId)) {
            impl_->mgr.addSession(roomId, senderKey, sessionId, sessionKey);
            impl_->params.push_back({roomId, senderKey, sessionId, sessionKey});
        }
    }
    LOG(LogChannel::E2EE, "unpickleAll: loaded %d sessions",
        impl_->mgr.sessionCount());
    return true;
}

int MegolmStore::sessionCount() {
    std::lock_guard<std::mutex> lk(mtx_);
    return impl_->mgr.sessionCount();
}

int MegolmStore::pendingCount() {
    std::lock_guard<std::mutex> lk(mtx_);
    return static_cast<int>(pending_.size());
}

} // namespace progressive::desktop

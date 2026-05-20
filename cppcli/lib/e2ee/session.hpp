#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>
#include "olm.hpp"

namespace matrixcli { namespace e2ee {

using json = nlohmann::json;

struct OlmSessionData {
    std::string session_id;
    std::string identity_key;
    int64_t first_known_index = 0;
    int64_t message_index = 0;

    json toJSON() const;
    static OlmSessionData fromJSON(const json& j);
};

class OlmSessionStore {
public:
    void store(const std::string& identity_key, OlmSession& session) {
        _pickled[identity_key] = session.pickle("matrixcli");
    }

    OlmSession* get(const std::string& identity_key) {
        auto it = _pickled.find(identity_key);
        if (it != _pickled.end()) {
            _active[identity_key] = OlmSession::unpickle("matrixcli", it->second);
            _pickled.erase(it);
        }
        auto act = _active.find(identity_key);
        if (act != _active.end()) {
            return &act->second;
        }
        return nullptr;
    }

    bool has(const std::string& identity_key) const {
        return _active.contains(identity_key) || _pickled.contains(identity_key);
    }

    void remove(const std::string& identity_key) {
        _active.erase(identity_key);
        _pickled.erase(identity_key);
    }

    void pickleAll(const std::string& key) {
        for (auto& [ik, session] : _active) {
            _pickled[ik] = session.pickle(key);
        }
        _active.clear();
    }

    size_t size() const { return _active.size() + _pickled.size(); }

private:
    std::map<std::string, OlmSession> _active;
    std::map<std::string, std::string> _pickled;
};

}} // namespace matrixcli::e2ee

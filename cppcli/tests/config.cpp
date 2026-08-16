#include "config.hpp"

#include <fstream>
#include <filesystem>

namespace matrixcli {

namespace fs = std::filesystem;

Config& Config::instance() {
    static Config config;
    return config;
}

void Config::load(const std::string& path) {
    _path = path;

    if (fs::exists(path)) {
        std::ifstream file(path);
        if (file.is_open()) {
            try {
                _data = nlohmann::json::parse(file);
            } catch (...) {
                _data = nlohmann::json::object();
            }
        }
    } else {
        _data = nlohmann::json::object();
    }
}

void Config::save(const std::string& path) {
    std::string save_path = path.empty() ? _path : path;
    if (save_path.empty()) return;

    auto parent = fs::path(save_path).parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        fs::create_directories(parent);
    }

    std::ofstream file(save_path);
    if (file.is_open()) {
        file << _data.dump(2);
    }
}

std::string Config::get(const std::string& key, const std::string& default_val) const {
    if (_data.contains(key)) {
        if (_data[key].is_string()) {
            return _data[key].get<std::string>();
        }
        return _data[key].dump();
    }
    return default_val;
}

void Config::set(const std::string& key, const std::string& value) {
    _data[key] = value;
}

nlohmann::json Config::filters() const {
    try {
        auto raw = get("filters", "{}");
        auto j = nlohmann::json::parse(raw);
        return j.is_object() ? j : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

void Config::setFilters(const nlohmann::json& filters) {
    if (filters.is_object() && filters.empty()) { _data.erase("filters"); return; }
    set("filters", filters.is_object() ? filters.dump() : "{}");
}

std::vector<std::string> Config::hiddenAccounts() const {
    std::vector<std::string> out;
    try {
        auto j = nlohmann::json::parse(get("hidden_accounts", "[]"));
        if (j.is_array()) for (auto& v : j) if (v.is_string()) out.push_back(v.get<std::string>());
    } catch (...) {}
    return out;
}

void Config::setHiddenAccounts(const std::vector<std::string>& mxids) {
    if (mxids.empty()) { _data.erase("hidden_accounts"); return; }
    nlohmann::json arr = nlohmann::json::array();
    for (auto& m : mxids) arr.push_back(m);
    set("hidden_accounts", arr.dump());
}

std::string Config::homeserverURL() const {
    return get("homeserver_url");
}

std::string Config::accessToken() const {
    return get("access_token");
}

std::string Config::userId() const {
    return get("user_id");
}

std::string Config::deviceId() const {
    return get("device_id");
}

} // namespace matrixcli

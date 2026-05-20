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

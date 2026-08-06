#pragma once

#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace matrixcli {

class Config {
public:
    static Config& instance();

    void load(const std::string& path);
    void save(const std::string& path = "");

    std::string get(const std::string& key, const std::string& default_val = "") const;
    void set(const std::string& key, const std::string& value);

    std::string homeserverURL() const;
    std::string accessToken() const;
    std::string userId() const;
    std::string deviceId() const;

private:
    Config() = default;
    nlohmann::json _data;
    std::string _path;
};

} // namespace matrixcli

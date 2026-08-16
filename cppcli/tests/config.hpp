#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace matrixcli {

class Config {
public:
    static Config& instance();

    void load(const std::string& path);
    void save(const std::string& path = "");

    std::string get(const std::string& key, const std::string& default_val = "") const;
    void set(const std::string& key, const std::string& value);

    // Permanent view filters (stored under "filters" as a JSON string):
    //   {"senders": [...], "hide": [...], "rooms": {"!id": {"senders": [...], "hide": [...]}}}
    nlohmann::json filters() const;
    void setFilters(const nlohmann::json& filters);

    // Accounts hidden from the accounts list (stored under "hidden_accounts").
    std::vector<std::string> hiddenAccounts() const;
    void setHiddenAccounts(const std::vector<std::string>& mxids);

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

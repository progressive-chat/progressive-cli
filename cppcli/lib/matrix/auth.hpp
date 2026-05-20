#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>

#include <nlohmann/json.hpp>

namespace matrixcli { namespace matrix {

using json = nlohmann::json;

struct Credentials {
    std::string user_id;
    std::string access_token;
    std::string refresh_token;
    std::string device_id;
    std::string homeserver_url;

    bool valid() const {
        return !user_id.empty() && !access_token.empty();
    }
};

struct LoginFlow {
    std::string type;
    std::vector<std::string> stages;
};

struct LoginFlowsResult {
    std::vector<LoginFlow> flows;
};

struct WellKnownHomeserver {
    std::string base_url;
};

struct WellKnownIdentityServer {
    std::string base_url;
};

struct WellKnown {
    WellKnownHomeserver homeserver;
    WellKnownIdentityServer identity_server;

    bool valid() const { return !homeserver.base_url.empty(); }
    static WellKnown fromJson(const json& j);
};

struct ServerVersions {
    std::vector<std::string> versions;
    bool supports(const std::string& version) const;

    static ServerVersions fromJson(const json& j);
};

struct SessionInfo {
    std::string user_id;
    std::string device_id;
    bool is_guest = false;

    static SessionInfo fromJson(const json& j);
};

LoginFlowsResult parseLoginFlows(const std::string& json_str);
Credentials parseCredentials(const std::string& json_str);

}} // namespace matrixcli::matrix

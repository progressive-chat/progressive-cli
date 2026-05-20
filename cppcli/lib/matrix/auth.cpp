#include "auth.hpp"
#include <sstream>

namespace matrixcli { namespace matrix {

LoginFlowsResult parseLoginFlows(const std::string& json_str) {
    LoginFlowsResult result;
    try {
        auto j = json::parse(json_str);
        if (j.contains("flows")) {
            for (auto& flow : j["flows"]) {
                LoginFlow f;
                f.type = flow.value("type", "");
                if (flow.contains("stages")) {
                    for (auto& stage : flow["stages"])
                        f.stages.push_back(stage.get<std::string>());
                }
                result.flows.push_back(f);
            }
        }
    } catch (...) {}
    return result;
}

Credentials parseCredentials(const std::string& json_str) {
    Credentials creds;
    try {
        auto j = json::parse(json_str);
        creds.user_id = j.value("user_id", "");
        creds.access_token = j.value("access_token", "");
        creds.refresh_token = j.value("refresh_token", "");
        creds.device_id = j.value("device_id", "");

        if (j.contains("well_known")) {
            auto& wk = j["well_known"];
            if (wk.contains("m.homeserver"))
                creds.homeserver_url = wk["m.homeserver"].value("base_url", "");
        }
    } catch (...) {}
    return creds;
}

WellKnown WellKnown::fromJson(const json& j) {
    WellKnown wk;
    if (j.contains("m.homeserver"))
        wk.homeserver.base_url = j["m.homeserver"].value("base_url", "");
    if (j.contains("m.identity_server"))
        wk.identity_server.base_url = j["m.identity_server"].value("base_url", "");
    return wk;
}

ServerVersions ServerVersions::fromJson(const json& j) {
    ServerVersions sv;
    if (j.contains("versions")) {
        for (auto& v : j["versions"])
            sv.versions.push_back(v.get<std::string>());
    }
    return sv;
}

bool ServerVersions::supports(const std::string& version) const {
    for (const auto& v : versions) {
        if (v == version) return true;
    }
    return false;
}

SessionInfo SessionInfo::fromJson(const json& j) {
    SessionInfo si;
    si.user_id = j.value("user_id", "");
    si.device_id = j.value("device_id", "");
    si.is_guest = j.value("is_guest", false);
    return si;
}

}} // namespace matrixcli::matrix

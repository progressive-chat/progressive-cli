// client_discovery.cpp — server discovery, account data, push rules and
// third-party / capabilities / OpenID / TURN helpers for matrixcli::matrix::Client.
#include "client.hpp"
#include "error.hpp"
#include "../database/db.hpp"
#include "../util/logger.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;

#include "client_impl.hpp"

namespace matrixcli { namespace matrix {

json Client::getDevices() {
    auto resp = authGet("/_matrix/client/r0/devices");
    checkResponse(resp);
    return json::parse(resp.body);
}

bool Client::deleteDevices(const std::vector<std::string>& device_ids) {
    json body = {{"devices", device_ids}};
    auto resp = authPost("/_matrix/client/r0/delete_devices", body.dump());
    return resp.ok();
}

json Client::getPushRules() {
    auto resp = authGet("/_matrix/client/r0/pushrules");
    checkResponse(resp);
    return json::parse(resp.body);
}

std::string Client::createFilter(const std::string& filter_json) {
    auto resp = authPost("/_matrix/client/r0/user/" +
                         http::urlEncode(impl->creds.user_id) + "/filter",
                         filter_json);
    checkResponse(resp);
    auto j = json::parse(resp.body);
    return j["filter_id"].get<std::string>();
}

json Client::getTurnServer() {
    auto resp = authGet("/_matrix/client/v3/voip/turnServer");
    if (!resp.ok()) return json::object();
    return json::parse(resp.body);
}

json Client::getOpenIdToken() {
    auto resp = authPost("/_matrix/client/v3/user/" +
                         http::urlEncode(impl->creds.user_id) +
                         "/openid/request_token", "{}");
    if (!resp.ok()) return json::object();
    return json::parse(resp.body);
}

json Client::getCapabilities() {
    auto resp = authGet("/_matrix/client/v3/capabilities");
    if (!resp.ok()) return json::object();
    return json::parse(resp.body);
}

json Client::getThirdpartyProtocols() {
    auto resp = authGet("/_matrix/client/r0/thirdparty/protocols");
    if (!resp.ok()) return json::object();
    return json::parse(resp.body);
}

json Client::getThirdpartyUsers(const std::string& protocol, const std::string& network_id) {
    std::map<std::string, std::string> params;
    if (!network_id.empty()) params["network_id"] = network_id;
    auto resp = authGet("/_matrix/client/r0/thirdparty/user/" + http::urlEncode(protocol), params);
    if (!resp.ok()) return json::array();
    return json::parse(resp.body);
}

json Client::getThirdpartyLocations(const std::string& protocol, const std::string& network_id) {
    std::map<std::string, std::string> params;
    if (!network_id.empty()) params["network_id"] = network_id;
    auto resp = authGet("/_matrix/client/r0/thirdparty/location/" + http::urlEncode(protocol), params);
    if (!resp.ok()) return json::array();
    return json::parse(resp.body);
}

json Client::getAccountData(const std::string& type) {
    auto resp = authGet("/_matrix/client/r0/user/" +
                        http::urlEncode(impl->creds.user_id) + "/account_data/" +
                        http::urlEncode(type));
    if (!resp.ok()) return json::object();
    return json::parse(resp.body);
}

bool Client::setAccountData(const std::string& type, const json& content) {
    auto resp = authPut("/_matrix/client/r0/user/" +
                        http::urlEncode(impl->creds.user_id) + "/account_data/" +
                        http::urlEncode(type), content.dump());
    return resp.ok();
}


}} // namespace matrixcli::matrix

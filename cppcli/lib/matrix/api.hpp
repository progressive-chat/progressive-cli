#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "client.hpp"

namespace matrixcli { namespace matrix { namespace api {

using json = nlohmann::json;

LoginFlowsResult getLoginFlows(http::Client& http, const std::string& server);

json doLogin(http::Client& http, const std::string& server, const json& body);

std::string sendMessage(http::Client& http, const Credentials& creds,
                         const std::string& room_id, const std::string& body,
                         const std::string& msgtype);

json sync(http::Client& http, const Credentials& creds,
          const std::string& since = "");

std::string uploadFile(http::Client& http, const Credentials& creds,
                        const std::string& filename, const std::vector<uint8_t>& data);

json whoami(http::Client& http, const Credentials& creds);

}}} // namespace matrixcli::matrix::api

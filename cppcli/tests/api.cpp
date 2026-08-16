#include "api.hpp"
#include "error.hpp"
#include "../util/logger.hpp"

#include <stdexcept>

namespace matrixcli { namespace matrix { namespace api {

static std::string makeURL(const std::string& server, const std::string& path) {
    std::string base = server;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

LoginFlowsResult getLoginFlows(http::Client& http, const std::string& server) {
    auto url = makeURL(server, "/_matrix/client/v3/login");
    auto resp = http.get(url);
    if (resp.status_code != 200) {
        throw MatrixError("Failed to get login flows: HTTP " +
                          std::to_string(resp.status_code));
    }
    auto j = json::parse(resp.body);
    LoginFlowsResult result;
    if (j.contains("flows")) {
        for (auto& flow : j["flows"]) {
            LoginFlow f;
            if (flow.contains("type")) {
                f.type = flow["type"].get<std::string>();
            }
            result.flows.push_back(std::move(f));
        }
    }
    return result;
}

json doLogin(http::Client& http, const std::string& server, const json& body) {
    auto url = makeURL(server, "/_matrix/client/v3/login");
    auto resp = http.post(url, body.dump(), {{"Content-Type", "application/json"}});
    if (resp.status_code != 200) {
        throw MatrixError("Login failed: HTTP " + std::to_string(resp.status_code) +
                          " - " + resp.body);
    }
    return json::parse(resp.body);
}

std::string sendMessage(http::Client& http, const Credentials& creds,
                         const std::string& room_id, const std::string& body,
                         const std::string& msgtype) {
    auto url = makeURL(creds.homeserver_url,
                       "/_matrix/client/v3/rooms/" + room_id + "/send/m.room.message/" +
                       std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));

    json content = {
        {"msgtype", msgtype},
        {"body", body}
    };

    auto resp = http.put(url, content.dump(), {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + creds.access_token}
    });

    if (resp.status_code != 200) {
        throw MatrixError("Failed to send message: HTTP " +
                          std::to_string(resp.status_code));
    }

    auto j = json::parse(resp.body);
    return j.value("event_id", "");
}

json sync(http::Client& http, const Credentials& creds,
          const std::string& since) {
    std::string url = makeURL(creds.homeserver_url, "/_matrix/client/v3/sync");
    if (!since.empty()) {
        url += "?since=" + since + "&timeout=30000";
    }
    auto resp = http.get(url, {
        {"Authorization", "Bearer " + creds.access_token}
    });
    if (resp.status_code != 200) {
        throw MatrixError("Sync failed: HTTP " + std::to_string(resp.status_code));
    }
    return json::parse(resp.body);
}

std::string uploadFile(http::Client& http, const Credentials& creds,
                        const std::string& filename,
                        const std::vector<uint8_t>& data) {
    auto url = makeURL(creds.homeserver_url, "/_matrix/media/v3/upload?filename=" +
                       filename);
    std::string body(data.begin(), data.end());
    auto resp = http.post(url, body, {
        {"Authorization", "Bearer " + creds.access_token},
        {"Content-Type", "application/octet-stream"}
    });
    if (resp.status_code != 200) {
        throw MatrixError("Failed to upload file: HTTP " +
                          std::to_string(resp.status_code));
    }
    auto j = json::parse(resp.body);
    return j.value("content_uri", "");
}

json whoami(http::Client& http, const Credentials& creds) {
    auto url = makeURL(creds.homeserver_url, "/_matrix/client/v3/account/whoami");
    auto resp = http.get(url, {
        {"Authorization", "Bearer " + creds.access_token}
    });
    if (resp.status_code != 200) {
        throw MatrixError("whoami failed: HTTP " + std::to_string(resp.status_code));
    }
    return json::parse(resp.body);
}

}}} // namespace matrixcli::matrix::api

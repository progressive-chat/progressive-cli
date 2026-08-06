#include "handler.hpp"
#include <nlohmann/json.hpp>

namespace matrixcli { namespace api {

MatrixHandler::MatrixHandler(matrix::Client& client) : _client(client) {}

Response MatrixHandler::handleStatus(const Request& req) {
    Response resp;
    resp.content_type = "application/json";

    nlohmann::json j;
    j["logged_in"] = _client.isLoggedIn();
    if (_client.isLoggedIn()) {
        j["user_id"] = _client.userId();
    }
    resp.body = j.dump(2);
    return resp;
}

Response MatrixHandler::handleLogin(const Request& req) {
    Response resp;
    resp.content_type = "application/json";

    try {
        auto j = nlohmann::json::parse(req.body);
        std::string username = j.value("username", "");
        std::string password = j.value("password", "");
        std::string device_name = j.value("device_name", "matrixcli");

        auto creds = _client.loginPassword(username, password, device_name);

        nlohmann::json result;
        result["user_id"] = creds.user_id;
        result["device_id"] = creds.device_id;
        result["access_token"] = creds.access_token;
        resp.body = result.dump(2);
        resp.status = 200;
    } catch (const std::exception& e) {
        resp.status = 401;
        nlohmann::json err;
        err["error"] = e.what();
        resp.body = err.dump();
    }

    return resp;
}

Response MatrixHandler::handleSync(const Request& req) {
    Response resp;
    resp.content_type = "application/json";

    if (!_client.isLoggedIn()) {
        resp.status = 401;
        resp.body = R"({"error":"not logged in"})";
        return resp;
    }

    nlohmann::json j;
    j["status"] = "ok";
    j["message"] = "Sync is handled via the background loop. Use GET /events for events.";
    resp.body = j.dump(2);
    return resp;
}

Response MatrixHandler::handleSendMessage(const Request& req) {
    Response resp;
    resp.content_type = "application/json";

    if (!_client.isLoggedIn()) {
        resp.status = 401;
        resp.body = R"({"error":"not logged in"})";
        return resp;
    }

    try {
        auto j = nlohmann::json::parse(req.body);
        std::string room_id = j.value("room_id", "");
        std::string body = j.value("body", "");
        std::string msgtype = j.value("msgtype", "m.text");

        auto event_id = _client.sendMessage(room_id, body, msgtype);

        nlohmann::json result;
        result["event_id"] = event_id;
        resp.body = result.dump(2);
        resp.status = 200;
    } catch (const std::exception& e) {
        resp.status = 400;
        nlohmann::json err;
        err["error"] = e.what();
        resp.body = err.dump();
    }

    return resp;
}

}} // namespace matrixcli::api

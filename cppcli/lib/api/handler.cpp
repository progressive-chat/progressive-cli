#include "handler.hpp"
#include <nlohmann/json.hpp>
#include <sstream>

namespace matrixcli { namespace api {

MatrixHandler::MatrixHandler(matrix::Client& client) : _client(client) {}

Response MatrixHandler::handleStatus(const Request& req) {
    Response resp;

    nlohmann::json j;
    j["logged_in"] = _client.isLoggedIn();
    if (_client.isLoggedIn()) {
        j["user_id"] = _client.userId();
    }

    auto fmt = req.format;
    if (fmt == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << "Progressive Chat CLI\n"
            << "══════════════════════════════════════\n"
            << "Logged in: " << (j["logged_in"].get<bool>() ? "yes" : "no") << "\n";
        if (_client.isLoggedIn()) oss << "User:      " << j["user_id"].get<std::string>() << "\n";
        resp.body = oss.str();
    } else if (fmt == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# Status\n\n"
            << "- **logged_in**: " << (j["logged_in"].get<bool>() ? "yes" : "no") << "\n";
        if (_client.isLoggedIn()) oss << "- **user_id**: `" << j["user_id"].get<std::string>() << "`\n";
        resp.body = oss.str();
    } else if (fmt == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# Status\n\n"
            << (j["logged_in"].get<bool>() ? "@" + j["user_id"].get<std::string>() : "not logged in") << "\n";
        resp.body = oss.str();
    } else if (fmt == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Status</title>"
            << "<style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".v{color:#ce9178}</style></head><body><h1>Status</h1><p class='v'>"
            << (j["logged_in"].get<bool>() ? "logged in as " + j["user_id"].get<std::string>() : "not logged in")
            << "</p></body></html>";
        resp.body = oss.str();
    } else {
        resp.content_type = "application/json";
        resp.body = j.dump(2);
    }
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

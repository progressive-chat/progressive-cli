#include "handler.hpp"
#include <nlohmann/json.hpp>
#include <sstream>

namespace matrixcli { namespace api {

MatrixHandler::MatrixHandler(progressive::desktop::MatrixClient& client) : _client(client) {}

Response MatrixHandler::handleStatus(const Request& req) {
    Response resp;

    nlohmann::json j;
    j["logged_in"] = _client.isLoggedIn();
    if (_client.isLoggedIn()) {
        auto acct = _client.account();
        j["user_id"] = acct.userId;
        j["device_id"] = acct.deviceId;
        j["homeserver"] = acct.homeserverUrl;
    }

    auto fmt = req.format;
    if (fmt == Format::Text) {
        resp.content_type = "text/plain";
        std::ostringstream oss;
        oss << "Progressive Chat CLI\n"
            << "══════════════════════════════════════\n"
            << "Logged in: " << (j["logged_in"].get<bool>() ? "yes" : "no") << "\n";
        if (_client.isLoggedIn()) {
            auto acct = _client.account();
            oss << "User:      " << acct.userId << "\n"
                << "Device:    " << acct.deviceId << "\n"
                << "Server:    " << acct.homeserverUrl << "\n";
        }
        resp.body = oss.str();
    } else if (fmt == Format::Markdown) {
        resp.content_type = "text/markdown";
        std::ostringstream oss;
        oss << "# Status\n\n"
            << "- **logged_in**: " << (j["logged_in"].get<bool>() ? "yes" : "no") << "\n";
        if (_client.isLoggedIn()) {
            auto acct = _client.account();
            oss << "- **user_id**: `" << acct.userId << "`\n"
                << "- **device_id**: `" << acct.deviceId << "`\n";
        }
        resp.body = oss.str();
    } else if (fmt == Format::Gemini) {
        resp.content_type = "text/gemini";
        std::ostringstream oss;
        oss << "# Status\n\n"
            << (_client.isLoggedIn() ? "@" + _client.account().userId : "not logged in") << "\n";
        resp.body = oss.str();
    } else if (fmt == Format::HTML) {
        resp.content_type = "text/html";
        std::ostringstream oss;
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Status</title>"
            << "<style>body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;padding:20px;}"
            << ".v{color:#ce9178}</style></head><body><h1>Status</h1><p class='v'>"
            << (_client.isLoggedIn() ? "logged in as " + _client.account().userId : "not logged in")
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

        auto r = _client.loginWithPassword(username, password, device_name);
        if (!r.ok) {
            resp.status = 401;
            nlohmann::json err;
            err["error"] = r.error.message.empty() ? "login failed" : r.error.message;
            if (!r.error.code.empty()) err["errcode"] = r.error.code;
            resp.body = err.dump();
            return resp;
        }

        _client.persistSession();
        auto acct = _client.account();
        nlohmann::json result;
        result["user_id"] = acct.userId;
        result["device_id"] = acct.deviceId;
        result["access_token"] = acct.accessToken;
        result["homeserver"] = acct.homeserverUrl;
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

        auto r = _client.sendMessage(room_id, body, msgtype);

        nlohmann::json result;
        if (r.ok) {
            result["event_id"] = r.data;
            resp.body = result.dump(2);
            resp.status = 200;
        } else {
            resp.status = 400;
            result["error"] = r.error.message.empty() ? "send failed" : r.error.message;
            resp.body = result.dump();
        }
    } catch (const std::exception& e) {
        resp.status = 400;
        nlohmann::json err;
        err["error"] = e.what();
        resp.body = err.dump();
    }

    return resp;
}

}} // namespace matrixcli::api

// gomuks_client.cpp — HTTP auth + jsoncmd request/response correlation
// over WsClient. Request ids count up from 1; server pushes carry
// request_id 0 and are routed to the event handler, including while a
// call() is waiting for its own response.
#include "gomuks_client.hpp"

#include <chrono>

#include "jsoncmd.hpp"
#include "../http/http.hpp"

namespace matrixcli { namespace gomuks {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string stripTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// Derive the websocket URL from the backend base URL:
// http(s)://host[:port] -> ws(s)://host[:port]/_gomuks/websocket
bool wsUrlFor(const std::string& baseUrl, std::string& wsUrl,
              std::string& error) {
    std::string scheme, rest;
    auto sep = baseUrl.find("://");
    if (sep == std::string::npos) {
        error = "backend URL needs an http:// or https:// scheme";
        return false;
    }
    scheme = baseUrl.substr(0, sep);
    rest = baseUrl.substr(sep + 3);
    if (scheme != "http" && scheme != "https") {
        error = "backend URL must be http(s):// (the ws upgrade is derived)";
        return false;
    }
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    wsUrl = (scheme == "https" ? "wss://" : "ws://") + rest +
            "/_gomuks/websocket";
    return true;
}

} // namespace

bool GomuksClient::login(const std::string& baseUrl,
                         const std::string& username,
                         const std::string& password, std::string& error) {
    _baseUrl = stripTrailingSlash(baseUrl);
    _cookie.clear();
    http::Client http;
    http.setTimeout(15);
    std::map<std::string, std::string> headers;
    headers["Authorization"] =
        "Basic " + base64Encode(username + ":" + password);
    http::Response resp =
        http.post(_baseUrl + "/_gomuks/auth?output=json", "",
                  headers);
    if (resp.status_code == 401) {
        error = "gomuks backend rejected the credentials (HTTP 401)";
        return false;
    }
    if (!resp.ok()) {
        error = "gomuks auth failed (HTTP " +
                std::to_string(resp.status_code) + ")";
        if (!resp.error_message.empty()) error += ": " + resp.error_message;
        return false;
    }
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (!j.contains("token") || !j["token"].is_string() ||
            j["token"].get<std::string>().empty()) {
            error = "gomuks auth reply carries no token";
            return false;
        }
        _cookie = j["token"].get<std::string>();
    } catch (const std::exception& e) {
        error = std::string("gomuks auth reply is not JSON: ") + e.what();
        return false;
    }
    return true;
}

void GomuksClient::setSession(const std::string& baseUrl,
                              const std::string& cookie) {
    _baseUrl = stripTrailingSlash(baseUrl);
    _cookie = cookie;
}

bool GomuksClient::connect(int timeoutSec, std::string& error) {
    if (_baseUrl.empty() || _cookie.empty()) {
        error = "no gomuks session: login() or setSession() first";
        return false;
    }
    std::string wsUrl;
    if (!wsUrlFor(_baseUrl, wsUrl, error)) return false;
    std::map<std::string, std::string> headers;
    headers["Cookie"] = "gomuks_auth=" + _cookie;
    if (!_ws.connect(wsUrl, headers, timeoutSec, error)) {
        error = "gomuks websocket: " + error;
        return false;
    }
    return true;
}

bool GomuksClient::recvOne(std::string& command, nlohmann::json& data,
                           int64_t& requestId, int timeoutSec,
                           std::string& error) {
    std::string text;
    if (!_ws.recvText(text, timeoutSec)) {
        error = _ws.lastError();
        return false;
    }
    JsonEnvelope env;
    if (!parseEnvelope(text, env, error)) return false;
    command = env.command;
    data = env.data;
    requestId = env.requestId;
    return true;
}

bool GomuksClient::call(const std::string& command,
                        const nlohmann::json& data,
                        nlohmann::json& responseData, int timeoutSec,
                        std::string& error) {
    if (!isConnected()) {
        error = "gomuks websocket not connected";
        return false;
    }
    const int64_t id = ++_nextId;
    std::string wire = formatCommand(command, id, data);
    if (!_ws.sendText(wire, error)) return false;
    const int64_t deadline = nowMs() + int64_t(timeoutSec) * 1000;
    for (;;) {
        int remaining =
            static_cast<int>((deadline - nowMs() + 999) / 1000);
        if (remaining <= 0) {
            error = "gomuks call '" + command + "' timed out";
            return false;
        }
        std::string evtCommand;
        nlohmann::json evtData;
        int64_t evtId = 0;
        if (!recvOne(evtCommand, evtData, evtId, remaining, error))
            return false;
        if (evtId == id) {
            if (evtCommand == cmd::kResponse) {
                responseData = evtData;
                return true;
            }
            if (evtCommand == cmd::kError) {
                error = "gomuks backend error: " +
                        (evtData.is_string()
                             ? evtData.get<std::string>()
                             : evtData.dump());
                return false;
            }
            error = "gomuks backend sent unexpected '" + evtCommand +
                    "' for request " + std::to_string(id);
            return false;
        }
        if (_onEvent) _onEvent(evtCommand, evtData);
    }
}

bool GomuksClient::ping(int64_t lastReceivedId, int timeoutSec,
                        std::string& error) {
    nlohmann::json data;
    data["last_received_id"] = lastReceivedId;
    const int64_t id = ++_nextId;
    if (!_ws.sendText(formatCommand(cmd::kPing, id, data), error))
        return false;
    const int64_t deadline = nowMs() + int64_t(timeoutSec) * 1000;
    for (;;) {
        int remaining =
            static_cast<int>((deadline - nowMs() + 999) / 1000);
        if (remaining <= 0) {
            error = "gomuks ping timed out";
            return false;
        }
        std::string evtCommand;
        nlohmann::json evtData;
        int64_t evtId = 0;
        if (!recvOne(evtCommand, evtData, evtId, remaining, error))
            return false;
        if (evtId == id) {
            if (evtCommand == cmd::kPong) return true;
            error = "gomuks backend sent unexpected '" + evtCommand +
                    "' for ping";
            return false;
        }
        if (_onEvent) _onEvent(evtCommand, evtData);
    }
}

bool GomuksClient::getState(nlohmann::json& state, int timeoutSec,
                            std::string& error) {
    // get_state takes no payload; the backend answers {"command":
    // "response", ...} with the ClientState object.
    return call(cmd::kGetState, nlohmann::json::object(), state, timeoutSec,
                error);
}

bool GomuksClient::nextEvent(std::string& command, nlohmann::json& data,
                             int timeoutSec) {
    std::string error;
    int64_t requestId = 0;
    if (!recvOne(command, data, requestId, timeoutSec, error)) return false;
    if (requestId != 0) {
        // A late response with no waiter: surface it as an event so the
        // caller still sees every frame.
        data["request_id"] = requestId;
    }
    return true;
}

}} // namespace matrixcli::gomuks

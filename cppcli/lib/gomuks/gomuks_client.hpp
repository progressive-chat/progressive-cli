#pragma once
// Session against a gomuks backend (go.mau.fi/gomuks, pinned v0.2608.x):
// HTTP auth first, then the jsoncmd websocket. M1 surface: login,
// connect, ping/pong keepalive and get_state. Server-pushed frames
// (request_id 0: run_id, client_state, sync_status, sync_complete, ...)
// are delivered to the event handler.
#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "ws_client.hpp"

namespace matrixcli { namespace gomuks {

class GomuksClient {
public:
    using EventHandler =
        std::function<void(const std::string& command,
                           const nlohmann::json& data)>;

    // POST {baseUrl}/_gomuks/auth?output=json with HTTP Basic; stores the
    // gomuks_auth token for connect(). baseUrl like http://127.0.0.1:8080.
    bool login(const std::string& baseUrl, const std::string& username,
               const std::string& password, std::string& error);

    // For tests and token reuse: skip login() with an existing cookie value
    // (the raw gomuks_auth token, without the "gomuks_auth=" prefix).
    void setSession(const std::string& baseUrl, const std::string& cookie);

    // Open the websocket (GET /_gomuks/websocket with the auth cookie).
    bool connect(int timeoutSec, std::string& error);
    bool isConnected() const { return _ws.isOpen(); }

    // Send {command, request_id, data} and wait for the matching response
    // ("response" ok, "error" fail). Unmatched server events seen while
    // waiting go to the event handler.
    bool call(const std::string& command, const nlohmann::json& data,
              nlohmann::json& responseData, int timeoutSec,
              std::string& error);

    // jsoncmd ping (NOT the websocket frame ping): the backend answers
    // {"command":"pong"} and records last_received_id for resume.
    bool ping(int64_t lastReceivedId, int timeoutSec, std::string& error);

    bool getState(nlohmann::json& state, int timeoutSec,
                  std::string& error);

    // Pull one server-pushed event frame (anything with request_id 0 or
    // without a waiter). False on timeout.
    bool nextEvent(std::string& command, nlohmann::json& data,
                   int timeoutSec);

    void setEventHandler(EventHandler handler) { _onEvent = handler; }
    void disconnect() { _ws.close(); }

    std::string baseUrl() const { return _baseUrl; }
    // The bearer cookie (gomuks_auth token). Only hand it to the
    // configured backend or the local config store — never log it.
    std::string authCookie() const { return _cookie; }

private:
    bool recvOne(std::string& command, nlohmann::json& data,
                 int64_t& requestId, int timeoutSec, std::string& error);

    WsClient _ws;
    EventHandler _onEvent;
    std::string _baseUrl;
    std::string _cookie;
    int64_t _nextId = 0;
};

}} // namespace matrixcli::gomuks

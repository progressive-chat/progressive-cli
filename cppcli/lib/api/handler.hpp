#pragma once

#include "server.hpp"
#include "../ecore/core/matrix_client.hpp"

namespace matrixcli { namespace api {

// Matrix-mode API handler backed by the vendored desktop core (lib/ecore).
class MatrixHandler {
public:
    explicit MatrixHandler(progressive::desktop::MatrixClient& client);

    Response handleStatus(const Request& req);
    Response handleLogin(const Request& req);
    Response handleSync(const Request& req);
    Response handleSendMessage(const Request& req);

private:
    progressive::desktop::MatrixClient& _client;
};

}} // namespace matrixcli::api

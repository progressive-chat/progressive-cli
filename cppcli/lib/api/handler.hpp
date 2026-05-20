#pragma once

#include "server.hpp"
#include "../../matrix/client.hpp"

namespace matrixcli { namespace api {

class MatrixHandler {
public:
    explicit MatrixHandler(matrix::Client& client);

    Response handleStatus(const Request& req);
    Response handleLogin(const Request& req);
    Response handleSync(const Request& req);
    Response handleSendMessage(const Request& req);

private:
    matrix::Client& _client;
};

}} // namespace matrixcli::api

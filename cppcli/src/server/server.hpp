#pragma once

#include "../../lib/api/server.hpp"
#include "../../lib/api/router.hpp"
#include "../../lib/api/handler.hpp"
#include "../../lib/matrix/client.hpp"

namespace matrixcli { namespace server {

class APIServer {
public:
    explicit APIServer(int port);
    ~APIServer();

    void start();
    void stop();

    matrix::Client& client() { return _client; }

private:
    int _port;
    matrix::Client _client;
    api::Server _server;
};

}} // namespace matrixcli::server

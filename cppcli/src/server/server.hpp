#pragma once

#include "../../lib/api/server.hpp"
#include "../../lib/api/router.hpp"
#include "../../lib/api/handler.hpp"
#include "../../lib/api/demo_handler.hpp"
#include "../../lib/matrix/client.hpp"

namespace matrixcli { namespace server {

class APIServer {
public:
    explicit APIServer(int port, bool demo_mode = false);
    ~APIServer();

    void start();
    void stop();

    matrix::Client& client() { return _client; }

private:
    int _port;
    bool _demo;
    matrix::Client _client;
    api::Server _server;
};

}} // namespace matrixcli::server

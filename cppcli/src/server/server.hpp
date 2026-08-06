#pragma once

#include "../../lib/api/server.hpp"
#include "../../lib/api/router.hpp"
#include "../../lib/api/handler.hpp"
#include "../../lib/api/demo_handler.hpp"
#include "../../lib/matrix/client.hpp"
#include "web_proxy.hpp"
#include <memory>

namespace matrixcli { namespace server {

enum class ServerMode { Matrix, Demo, WebProxy };

class APIServer {
public:
    APIServer(int port, ServerMode mode = ServerMode::WebProxy,
              const std::string& homeserver = "https://matrix.org");
    ~APIServer();

    void start();
    void stop();

    matrix::Client& client() { return _client; }

private:
    int _port;
    ServerMode _mode;
    matrix::Client _client;
    api::Server _server;
    std::shared_ptr<api::DemoHandler> _demoHandler;
    std::unique_ptr<WebProxyHandler> _proxyHandler;
};

}} // namespace matrixcli::server

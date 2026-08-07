#pragma once

#include "../../lib/api/server.hpp"
#include "../../lib/api/router.hpp"
#include "../../lib/api/handler.hpp"
#include "../../lib/api/demo_handler.hpp"
#include "../../lib/ecore/core/matrix_client.hpp"
#include "web_proxy.hpp"
#include <memory>

namespace matrixcli { namespace server {

enum class ServerMode { Matrix, Demo, WebProxy };

class APIServer {
public:
    APIServer(int port, ServerMode mode = ServerMode::WebProxy,
              const std::string& homeserver = "https://matrix.org",
              std::shared_ptr<progressive::desktop::MatrixClient> client = nullptr);
    ~APIServer();

    void start();
    void stop();

private:
    int _port;
    ServerMode _mode;
    std::shared_ptr<progressive::desktop::MatrixClient> _client;
    api::Server _server;
    std::shared_ptr<api::DemoHandler> _demoHandler;
    std::unique_ptr<WebProxyHandler> _proxyHandler;
};

}} // namespace matrixcli::server

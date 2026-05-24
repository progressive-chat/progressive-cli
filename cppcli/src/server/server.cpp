#include "server.hpp"
#include "../../lib/util/logger.hpp"
#include "../../lib/api/router.hpp"
#include "../../lib/api/handler.hpp"
#include "../../lib/api/demo_handler.hpp"
#include "web_proxy.hpp"

namespace matrixcli { namespace server {

APIServer::APIServer(int port, ServerMode mode, const std::string& homeserver)
    : _port(port), _server(port), _mode(mode) {
    api::Router router;

    if (_mode == ServerMode::Demo) {
        _demoHandler = std::make_shared<api::DemoHandler>();
        _demoHandler->setPersistPath("demo.json");
        _demoHandler->load();
        _demoHandler->registerRoutes(router);
        util::Logger::instance().info("Demo mode enabled — no Matrix account required");
    } else if (_mode == ServerMode::Matrix) {
        api::MatrixHandler handler(_client);
        router.get("/api/status", [&](const api::Request& req) { return handler.handleStatus(req); });
        router.post("/api/login", [&](const api::Request& req) { return handler.handleLogin(req); });
        router.get("/api/sync", [&](const api::Request& req) { return handler.handleSync(req); });
        router.post("/api/send", [&](const api::Request& req) { return handler.handleSendMessage(req); });
        util::Logger::instance().info("Matrix mode — using saved credentials");
    } else {
        // WebProxy: stateless — token lives in browser, server is a proxy
        _proxyHandler = std::make_unique<WebProxyHandler>(homeserver);
        _proxyHandler->registerRoutes(router);
        util::Logger::instance().info("Web proxy mode — stateless, token in browser, homeserver: " + homeserver);
    }

    router.apply(_server);
}

APIServer::~APIServer() {
    stop();
    if (_demoHandler) _demoHandler->save();
}

void APIServer::start() {
    _server.start();
    util::Logger::instance().info("API server started on port " + std::to_string(_port));
}

void APIServer::stop() {
    _server.stop();
}

}} // namespace matrixcli::server

#include "server.hpp"
#include "../../lib/util/logger.hpp"
#include "../../lib/api/router.hpp"
#include "../../lib/api/handler.hpp"

namespace matrixcli { namespace server {

APIServer::APIServer(int port) : _port(port), _server(port) {
    api::Router router;
    api::MatrixHandler handler(_client);

    router.get("/api/status", [&](const api::Request& req) {
        return handler.handleStatus(req);
    });
    router.post("/api/login", [&](const api::Request& req) {
        return handler.handleLogin(req);
    });
    router.get("/api/sync", [&](const api::Request& req) {
        return handler.handleSync(req);
    });
    router.post("/api/send", [&](const api::Request& req) {
        return handler.handleSendMessage(req);
    });

    router.apply(_server);
}

APIServer::~APIServer() { stop(); }

void APIServer::start() {
    _server.start();
    util::Logger::instance().info("API server started on port " + std::to_string(_port));
}

void APIServer::stop() {
    _server.stop();
}

}} // namespace matrixcli::server

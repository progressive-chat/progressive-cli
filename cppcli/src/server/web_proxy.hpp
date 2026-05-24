#pragma once

#include "../../lib/api/server.hpp"
#include "../../lib/api/router.hpp"
#include <string>

namespace matrixcli { namespace server {

// Stateless web proxy — token in browser, server has no credentials
class WebProxyHandler {
public:
    explicit WebProxyHandler(const std::string& homeserver_url);
    void registerRoutes(api::Router& router);

private:
    api::Response handleProxy(const api::Request& req);
    api::Response handleHealth(const api::Request& req);

    std::string _homeserver;
};

}} // namespace matrixcli::server

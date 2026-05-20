#include "router.hpp"

namespace matrixcli { namespace api {

void Router::addRoute(const std::string& method, const std::string& path, Handler handler) {
    _routes.push_back({method, path, std::move(handler)});
}

void Router::get(const std::string& path, Handler handler) {
    addRoute("GET", path, std::move(handler));
}

void Router::post(const std::string& path, Handler handler) {
    addRoute("POST", path, std::move(handler));
}

void Router::put(const std::string& path, Handler handler) {
    addRoute("PUT", path, std::move(handler));
}

void Router::apply(Server& server) const {
    for (auto& route : _routes) {
        server.route(route.method, route.path, route.handler);
    }
}

}} // namespace matrixcli::api

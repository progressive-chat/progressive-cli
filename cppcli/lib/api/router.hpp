#pragma once

#include <string>
#include <map>
#include <vector>
#include <functional>
#include "server.hpp"

namespace matrixcli { namespace api {

struct RouteEntry {
    std::string method;
    std::string path;
    Handler handler;
};

class Router {
public:
    void addRoute(const std::string& method, const std::string& path, Handler handler);
    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);
    void put(const std::string& path, Handler handler);

    void apply(Server& server) const;

private:
    std::vector<RouteEntry> _routes;
};

}} // namespace matrixcli::api

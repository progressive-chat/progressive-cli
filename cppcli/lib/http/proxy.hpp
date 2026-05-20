#pragma once

#include <string>
#include <sys/socket.h>

namespace matrixcli { namespace http {

struct ProxyConnection {
    int sock = -1;
    std::string error;
};

ProxyConnection connectProxy(const std::string& host, int port,
                             const std::string& target_host, int target_port);

}} // namespace matrixcli::http

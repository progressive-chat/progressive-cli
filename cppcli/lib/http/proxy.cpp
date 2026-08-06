#include "proxy.hpp"
#include "../util/logger.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <cstdint>

namespace matrixcli { namespace http {

ProxyConnection connectProxy(const std::string& host, int port,
                             const std::string& target_host, int target_port) {
    ProxyConnection result;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        result.error = "Failed to resolve proxy host: " + host;
        return result;
    }

    int sock = -1;
    for (auto* p = res; p != nullptr; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        result.error = "Failed to connect to proxy";
        return result;
    }

    std::ostringstream connect_request;
    connect_request << "CONNECT " << target_host << ":" << target_port << " HTTP/1.1\r\n";
    connect_request << "Host: " << target_host << ":" << target_port << "\r\n";
    connect_request << "\r\n";

    std::string req = connect_request.str();
    ::send(sock, req.data(), req.size(), 0);

    char buf[4096];
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        std::string response(buf);
        if (response.find("200") != std::string::npos) {
            result.sock = sock;
            return result;
        }
    }

    close(sock);
    result.error = "Proxy CONNECT rejected";
    return result;
}

}} // namespace matrixcli::http

#include "server.hpp"
#include "format.hpp"
#include "../util/logger.hpp"
#include "../util/string_utils.hpp"

#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace matrixcli { namespace api {

Server::Server(int port) : _port(port) {}

Server::~Server() { stop(); }

void Server::route(const std::string& method, const std::string& path, Handler handler) {
    _routes[{method, path}] = std::move(handler);
}

void Server::start() {
    if (_running.exchange(true)) return;

    _server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (_server_sock < 0) {
        throw std::runtime_error("Failed to create server socket");
    }

    int opt = 1;
    setsockopt(_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(_port);

    if (bind(_server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(_server_sock);
        throw std::runtime_error("Failed to bind to port " + std::to_string(_port));
    }

    if (listen(_server_sock, 10) < 0) {
        close(_server_sock);
        throw std::runtime_error("Failed to listen on port " + std::to_string(_port));
    }

    util::Logger::instance().info("API server listening on port " + std::to_string(_port));

    _thread = std::make_unique<std::thread>(&Server::acceptLoop, this);
}

void Server::stop() {
    if (!_running.exchange(false)) return;

    if (_server_sock >= 0) {
        shutdown(_server_sock, SHUT_RDWR);
        close(_server_sock);
        _server_sock = -1;
    }

    if (_thread && _thread->joinable()) {
        _thread->join();
    }
    _thread.reset();
}

void Server::acceptLoop() {
    while (_running.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(_server_sock, (struct sockaddr*)&client_addr, &client_len);

        if (client_sock < 0) {
            if (_running.load()) {
                util::Logger::instance().warn("accept failed");
            }
            continue;
        }

        std::thread(&Server::handleClient, this, client_sock).detach();
    }
}

void Server::handleClient(int client_sock) {
    std::string raw;
    char buf[8192];
    ssize_t n;
    while ((n = recv(client_sock, buf, sizeof(buf) - 1, 0)) > 0) {
        raw.append(buf, n);
        if (raw.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }

    if (!raw.empty()) {
        try {
            auto req = parseRequest(raw);
            auto handler = findHandler(req.method, req.path, req.params);
            Response resp;
            if (handler) {
                resp = handler(req);
            } else {
                resp.status = 404;
                resp.content_type = "application/json";
                resp.body = R"({"error":"not found"})";
            }
            std::string resp_str = buildResponse(resp);
            ::send(client_sock, resp_str.data(), resp_str.size(), 0);
        } catch (const std::exception& e) {
            std::string error_resp =
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: application/json\r\n"
                "Connection: close\r\n"
                "\r\n"
                R"({"error":"internal server error"})";
            ::send(client_sock, error_resp.data(), error_resp.size(), 0);
        }
    }

    shutdown(client_sock, SHUT_RDWR);
    close(client_sock);
}

Request Server::parseRequest(const std::string& raw) {
    Request req;
    std::istringstream iss(raw);
    std::string line;

    if (std::getline(iss, line)) {
        line = util::trim(line);
        auto parts = util::split(line, ' ');
        if (parts.size() >= 2) {
            req.method = parts[0];

            std::string full_path = parts[1];
            auto qpos = full_path.find('?');
            if (qpos != std::string::npos) {
                req.path = full_path.substr(0, qpos);
                std::string query = full_path.substr(qpos + 1);
                for (auto& param : util::split(query, '&')) {
                    auto eq = param.find('=');
                    if (eq != std::string::npos) {
                        req.params[param.substr(0, eq)] = param.substr(eq + 1);
                    } else {
                        req.params[param] = "";
                    }
                }
            } else {
                req.path = full_path;
            }
        }
    }

    while (std::getline(iss, line)) {
        line = util::trim(line);
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = util::trim(line.substr(0, colon));
            std::string value = util::trim(line.substr(colon + 1));
            req.headers[util::toLower(key)] = value;
        }
    }

    std::string remaining;
    while (std::getline(iss, line)) {
        remaining += line + "\n";
    }
    req.body = util::trim(remaining);

    auto accept_it = req.headers.find("accept");
    std::string accept = accept_it != req.headers.end() ? accept_it->second : "";
    auto fmt_param = req.params.find("format");
    std::string fmt = fmt_param != req.params.end() ? fmt_param->second : "";

    req.format = detectFormat(accept, fmt);

    return req;
}

std::string Server::buildResponse(const Response& resp) {
    std::string status_text;
    switch (resp.status) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 404: status_text = "Not Found"; break;
        case 500: status_text = "Internal Server Error"; break;
        default: status_text = "Unknown"; break;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << status_text << "\r\n";
    oss << "Content-Type: " << resp.content_type << "\r\n";
    oss << "Content-Length: " << resp.body.size() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << resp.body;
    return oss.str();
}

Handler Server::findHandler(const std::string& method, const std::string& path,
                              std::map<std::string, std::string>& pathParams) {
    // Exact match first
    auto it = _routes.find({method, path});
    if (it != _routes.end()) {
        return it->second;
    }

    // Try parameterized routes (paths containing ':param')
    auto req_parts = util::split(path, '/');
    for (auto& [key, handler] : _routes) {
        if (key.method != method) continue;

        auto route_parts = util::split(key.path, '/');
        if (req_parts.size() != route_parts.size()) continue;

        bool match = true;
        std::map<std::string, std::string> extracted;
        for (size_t i = 0; i < req_parts.size(); i++) {
            if (route_parts[i].starts_with(":")) {
                // C++26-style check for ':' prefix
                extracted[route_parts[i].substr(1)] = req_parts[i];
            } else if (route_parts[i] != req_parts[i]) {
                match = false;
                break;
            }
        }

        if (match) {
            for (auto& [k, v] : extracted) {
                pathParams[k] = v;
            }
            return handler;
        }
    }

    return nullptr;
}

}} // namespace matrixcli::api

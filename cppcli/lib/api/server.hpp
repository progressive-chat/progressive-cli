#pragma once

#include <string>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <sys/socket.h>

namespace matrixcli { namespace api {

enum class Format {
    JSON,
    Text,
    Markdown,
    Gemini,
    HTML
};

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> headers;
    std::string body;
    Format format = Format::JSON;
};

struct Response {
    int status = 200;
    std::string content_type;
    std::string body;
};

using Handler = std::function<Response(const Request&)>;

class Server {
public:
    explicit Server(int port = 8080);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void route(const std::string& method, const std::string& path, Handler handler);
    void start();
    void stop();
    bool isRunning() const { return _running.load(); }

private:
    void acceptLoop();
    void handleClient(int client_sock);
    Request parseRequest(const std::string& raw);
    std::string buildResponse(const Response& resp);
    Handler findHandler(const std::string& method, const std::string& path);

    int _port;
    int _server_sock = -1;
    std::atomic<bool> _running{false};
    std::unique_ptr<std::thread> _thread;

    struct RouteKey {
        std::string method;
        std::string path;
        bool operator<(const RouteKey& other) const {
            return std::tie(method, path) < std::tie(other.method, other.path);
        }
    };
    std::map<RouteKey, Handler> _routes;
};

}} // namespace matrixcli::api

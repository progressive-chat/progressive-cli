#pragma once

#include <string>
#include <map>
#include <memory>
#include <cstdint>
#include <functional>

namespace matrixcli { namespace http {

enum class ProxyType {
    None,
    HTTP,
    SOCKS5
};

struct ProxyConfig {
    ProxyType type = ProxyType::None;
    std::string host;
    int port = 0;
    std::string username;
    std::string password;

    bool enabled() const {
        return type != ProxyType::None && !host.empty() && port > 0;
    }
};

struct Request {
    std::string method;
    std::string url;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct Response {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error_message;

    bool ok() const { return status_code >= 200 && status_code < 300; }
    bool client_error() const { return status_code >= 400 && status_code < 500; }
    bool server_error() const { return status_code >= 500; }
};

struct RateLimitInfo {
    int limit = -1;
    int remaining = -1;
    int64_t reset_ms = 0;
};

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    void setProxy(const ProxyConfig& config);
    void setTimeout(int seconds);
    void setVerifyPeer(bool verify);

    Response get(const std::string& url,
                 const std::map<std::string, std::string>& headers = {});
    Response post(const std::string& url,
                  const std::string& body,
                  const std::map<std::string, std::string>& headers = {});
    Response put(const std::string& url,
                 const std::string& body,
                 const std::map<std::string, std::string>& headers = {});
    Response del(const std::string& url,
                 const std::map<std::string, std::string>& headers = {});

    Response doRequest(const Request& req);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

RateLimitInfo parseRateLimitHeaders(const Response& resp);
std::string urlEncode(const std::string& value);

}} // namespace matrixcli::http

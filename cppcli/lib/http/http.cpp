#include "http.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <regex>
#include <algorithm>
#include <mutex>
#include <thread>
#include <chrono>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace matrixcli { namespace http {

struct Client::Impl {
    ProxyConfig proxy;
    int timeout_seconds = 30;
    bool verify_peer = true;
    SSL_CTX* ssl_ctx = nullptr;
    mutable std::mutex mtx;

    Impl() {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ERR_load_crypto_strings();

        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) {
            throw std::runtime_error("Failed to create SSL_CTX");
        }

        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_default_verify_paths(ssl_ctx);

        SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
        SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    }

    ~Impl() {
        if (ssl_ctx) {
            SSL_CTX_free(ssl_ctx);
            ssl_ctx = nullptr;
        }
    }

    void updateVerifyMode() {
        if (!ssl_ctx) return;
        if (verify_peer) {
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);
        }
    }
};

Client::Client() : impl(std::make_unique<Impl>()) {}
Client::~Client() = default;

void Client::setProxy(const ProxyConfig& config) {
    std::lock_guard<std::mutex> lk(impl->mtx);
    impl->proxy = config;
}

void Client::setTimeout(int seconds) {
    std::lock_guard<std::mutex> lk(impl->mtx);
    impl->timeout_seconds = seconds;
}

void Client::setVerifyPeer(bool verify) {
    std::lock_guard<std::mutex> lk(impl->mtx);
    impl->verify_peer = verify;
    impl->updateVerifyMode();
}

static ProxyConfig readProxyFromEnv() {
    ProxyConfig cfg;
    const char* env = std::getenv("HTTPS_PROXY");
    if (!env || !*env) env = std::getenv("https_proxy");
    if (!env || !*env) env = std::getenv("ALL_PROXY");
    if (!env || !*env) env = std::getenv("all_proxy");
    if (!env || !*env) return cfg;

    std::string raw(env);
    std::regex re(R"(^(socks5|http)://(?:([^:@]+):([^@]+)@)?([^:]+):(\d+)/?$)");
    std::smatch m;
    if (std::regex_match(raw, m, re)) {
        std::string scheme = m[1].str();
        if (scheme == "socks5") cfg.type = ProxyType::SOCKS5;
        else cfg.type = ProxyType::HTTP;
        cfg.host = m[4].str();
        cfg.port = std::stoi(m[5].str());
        if (m[2].matched) cfg.username = m[2].str();
        if (m[3].matched) cfg.password = m[3].str();
    }
    return cfg;
}

static bool parseURL(const std::string& url,
                     std::string& host, int& port,
                     std::string& path, bool& use_ssl) {
    std::regex url_re(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, url_re)) return false;
    use_ssl = (m[1] == "https");
    host = m[2];
    port = m[3].matched ? std::stoi(m[3]) : (use_ssl ? 443 : 80);
    path = m[4].matched ? m[4].str() : "/";
    return true;
}

static bool setSocketTimeout(int sock, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return false;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return false;
    return true;
}

static int createTcpSocket(const std::string& host, int port, int timeout_seconds) {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(port);
    int gai_err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (gai_err != 0 || !res) return -1;

    int sock = -1;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;

        setSocketTimeout(sock, timeout_seconds);

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;

        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    return sock;
}

static bool sendAll(int sock, const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = send(sock, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

static bool recvAll(int sock, std::string& out, int timeout_seconds) {
    char buf[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);

    while (true) {
        int remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0) break;

        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        int poll_ret = poll(&pfd, 1, remaining_ms);
        if (poll_ret < 0) return false;
        if (poll_ret == 0) break;

        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

static bool socks5Handshake(int sock, const std::string& host, int port,
                            const std::string& username, const std::string& password) {
    bool has_auth = !username.empty();

    uint8_t greeting[3];
    size_t greet_len;
    if (has_auth) {
        greeting[0] = 0x05;
        greeting[1] = 0x02;
        greeting[2] = 0x00;
        greeting[3] = 0x02;
        greet_len = 4;
    } else {
        greeting[0] = 0x05;
        greeting[1] = 0x01;
        greeting[2] = 0x00;
        greet_len = 3;
    }

    if (!sendAll(sock, greeting, greet_len)) return false;

    uint8_t resp[2];
    if (recv(sock, resp, 2, MSG_WAITALL) != 2) return false;
    if (resp[0] != 0x05) return false;

    if (resp[1] == 0x02 && has_auth) {
        std::vector<uint8_t> auth_req;
        auth_req.push_back(0x01);
        auth_req.push_back(static_cast<uint8_t>(username.size()));
        auth_req.insert(auth_req.end(), username.begin(), username.end());
        auth_req.push_back(static_cast<uint8_t>(password.size()));
        auth_req.insert(auth_req.end(), password.begin(), password.end());
        if (!sendAll(sock, auth_req.data(), auth_req.size())) return false;
        uint8_t auth_resp[2];
        if (recv(sock, auth_resp, 2, MSG_WAITALL) != 2) return false;
        if (auth_resp[0] != 0x01 || auth_resp[1] != 0x00) return false;
    } else if (resp[1] != 0x00) {
        return false;
    }

    std::vector<uint8_t> conn_req;
    conn_req.push_back(0x05);
    conn_req.push_back(0x01);
    conn_req.push_back(0x00);

    struct addrinfo hints = {}, *ai = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai_err = getaddrinfo(host.c_str(), nullptr, &hints, &ai);
    if (gai_err == 0 && ai && ai->ai_family == AF_INET) {
        conn_req.push_back(0x01);
        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(ai->ai_addr);
        uint8_t* ip = reinterpret_cast<uint8_t*>(&sin->sin_addr.s_addr);
        conn_req.insert(conn_req.end(), ip, ip + 4);
    } else {
        conn_req.push_back(0x03);
        conn_req.push_back(static_cast<uint8_t>(host.size()));
        conn_req.insert(conn_req.end(), host.begin(), host.end());
    }
    if (ai) freeaddrinfo(ai);

    conn_req.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
    conn_req.push_back(static_cast<uint8_t>(port & 0xFF));

    if (!sendAll(sock, conn_req.data(), conn_req.size())) return false;

    uint8_t conn_resp[10];
    ssize_t cr_len = recv(sock, conn_resp, 10, MSG_WAITALL);
    if (cr_len < 10) return false;

    uint8_t atyp = conn_resp[3];
    size_t addr_len;
    if (atyp == 0x01) addr_len = 4;
    else if (atyp == 0x03) addr_len = conn_resp[4];
    else if (atyp == 0x04) addr_len = 16;
    else return false;

    size_t total = 6 + addr_len;
    if (cr_len < static_cast<ssize_t>(total)) {
        std::vector<uint8_t> rest(total - cr_len);
        if (recv(sock, rest.data(), rest.size(), MSG_WAITALL) != static_cast<ssize_t>(rest.size()))
            return false;
    }

    return conn_resp[1] == 0x00;
}

static bool httpConnectTunnel(int sock, const std::string& host, int port,
                              const std::string& username, const std::string& password) {
    std::ostringstream req;
    req << "CONNECT " << host << ":" << port << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";

    if (!username.empty()) {
        std::string creds = username + ":" + password;
        std::string encoded;
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        encoded.reserve(((creds.size() + 2) / 3) * 4);
        size_t i = 0;
        size_t len = creds.size();
        while (i < len) {
            uint32_t val = static_cast<uint8_t>(creds[i]) << 16;
            if (i + 1 < len) val |= static_cast<uint8_t>(creds[i + 1]) << 8;
            if (i + 2 < len) val |= static_cast<uint8_t>(creds[i + 2]);
            encoded += b64[(val >> 18) & 0x3F];
            encoded += b64[(val >> 12) & 0x3F];
            if (i + 1 < len) encoded += b64[(val >> 6) & 0x3F]; else encoded += '=';
            if (i + 2 < len) encoded += b64[val & 0x3F]; else encoded += '=';
            i += 3;
        }
        req << "Proxy-Authorization: Basic " << encoded << "\r\n";
    }

    req << "\r\n";

    if (!sendAll(sock, req.str().c_str(), req.str().size())) return false;

    std::string response;
    if (!recvAll(sock, response, 10)) return false;

    return response.find("200") != std::string::npos &&
           response.size() >= 12;
}

static std::string buildHttpRequest(const Request& req) {
    std::string host;
    int port;
    std::string path;
    bool use_ssl;
    if (!parseURL(req.url, host, port, path, use_ssl)) return "";

    std::ostringstream os;
    os << req.method << " " << path << " HTTP/1.1\r\n";
    os << "Host: " << host;
    if ((use_ssl && port != 443) || (!use_ssl && port != 80))
        os << ":" << port;
    os << "\r\n";

    for (const auto& [k, v] : req.headers)
        os << k << ": " << v << "\r\n";

    if (!req.body.empty())
        os << "Content-Length: " << req.body.size() << "\r\n";

    os << "Connection: close\r\n";
    os << "\r\n";

    if (!req.body.empty())
        os << req.body;

    return os.str();
}

static Response parseHttpResponse(const std::string& raw) {
    Response resp;

    auto bodyStart = raw.find("\r\n\r\n");
    if (bodyStart == std::string::npos) {
        resp.error_message = "Invalid HTTP response: no header/body separator";
        return resp;
    }

    std::string headerBlock = raw.substr(0, bodyStart);
    std::string rawBody = raw.substr(bodyStart + 4);

    auto firstNl = headerBlock.find("\r\n");
    if (firstNl == std::string::npos) {
        resp.error_message = "No status line in HTTP response";
        return resp;
    }
    std::string statusLine = headerBlock.substr(0, firstNl);

    auto codeStart = statusLine.find(' ');
    if (codeStart != std::string::npos) {
        auto codeEnd = statusLine.find(' ', codeStart + 1);
        std::string codeStr = (codeEnd != std::string::npos)
            ? statusLine.substr(codeStart + 1, codeEnd - codeStart - 1)
            : statusLine.substr(codeStart + 1);
        try {
            resp.status_code = std::stoi(codeStr);
        } catch (...) {
            resp.status_code = 0;
        }
    }

    size_t pos = firstNl + 2;
    while (pos < headerBlock.size()) {
        auto nl = headerBlock.find("\r\n", pos);
        if (nl == std::string::npos) break;
        std::string line = headerBlock.substr(pos, nl - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
                value = value.substr(1);
            resp.headers[key] = value;
        }
        pos = nl + 2;
    }

    auto te_it = resp.headers.find("Transfer-Encoding");
    if (te_it != resp.headers.end() && te_it->second.find("chunked") != std::string::npos) {
        std::string decoded;
        size_t p = 0;
        while (p < rawBody.size()) {
            auto crlf = rawBody.find("\r\n", p);
            if (crlf == std::string::npos) break;
            std::string hexLen = rawBody.substr(p, crlf - p);
            size_t chunkSize = 0;
            try { chunkSize = std::stoull(hexLen, nullptr, 16); }
            catch (...) { break; }
            if (chunkSize == 0) break;
            size_t dataStart = crlf + 2;
            if (dataStart + chunkSize > rawBody.size()) break;
            decoded.append(rawBody, dataStart, chunkSize);
            p = dataStart + chunkSize + 2;
        }
        resp.body = decoded;
    } else {
        resp.body = rawBody;
    }

    return resp;
}

Response Client::get(const std::string& url,
                     const std::map<std::string, std::string>& headers) {
    Request req;
    req.method = "GET";
    req.url = url;
    req.headers = headers;
    return doRequest(req);
}

Response Client::post(const std::string& url,
                      const std::string& body,
                      const std::map<std::string, std::string>& headers) {
    Request req;
    req.method = "POST";
    req.url = url;
    req.body = body;
    req.headers = headers;
    if (req.headers.find("Content-Type") == req.headers.end())
        req.headers["Content-Type"] = "application/json";
    return doRequest(req);
}

Response Client::put(const std::string& url,
                     const std::string& body,
                     const std::map<std::string, std::string>& headers) {
    Request req;
    req.method = "PUT";
    req.url = url;
    req.body = body;
    req.headers = headers;
    if (req.headers.find("Content-Type") == req.headers.end())
        req.headers["Content-Type"] = "application/json";
    return doRequest(req);
}

Response Client::del(const std::string& url,
                     const std::map<std::string, std::string>& headers) {
    Request req;
    req.method = "DELETE";
    req.url = url;
    req.headers = headers;
    return doRequest(req);
}

Response Client::doRequest(const Request& req) {
    Response resp;

    std::string host;
    int port;
    std::string path;
    bool use_ssl;
    if (!parseURL(req.url, host, port, path, use_ssl)) {
        resp.error_message = "Failed to parse URL: " + req.url;
        return resp;
    }

    ProxyConfig proxy;
    int timeout;
    SSL_CTX* ctx;
    {
        std::lock_guard<std::mutex> lk(impl->mtx);
        proxy = impl->proxy;
        timeout = impl->timeout_seconds;
        ctx = impl->ssl_ctx;

        if (!proxy.enabled()) {
            ProxyConfig env_proxy = readProxyFromEnv();
            if (env_proxy.enabled()) proxy = env_proxy;
        }
    }

    bool direct_tls = false;
    std::string connect_host = host;
    int connect_port = port;

    if (proxy.enabled()) {
        if (proxy.type == ProxyType::HTTP && use_ssl) {
            connect_host = proxy.host;
            connect_port = proxy.port;
        } else if (proxy.type == ProxyType::SOCKS5) {
            connect_host = proxy.host;
            connect_port = proxy.port;
        } else if (proxy.type == ProxyType::HTTP && !use_ssl) {
            connect_host = proxy.host;
            connect_port = proxy.port;
        }
    }

    int sock = createTcpSocket(connect_host, connect_port, timeout);
    if (sock < 0) {
        resp.error_message = "Failed to connect to " + connect_host + ":" + std::to_string(connect_port);
        return resp;
    }

    if (proxy.enabled()) {
        if (proxy.type == ProxyType::HTTP) {
            if (use_ssl) {
                if (!httpConnectTunnel(sock, host, port, proxy.username, proxy.password)) {
                    close(sock);
                    resp.error_message = "HTTP CONNECT tunnel failed";
                    return resp;
                }
            }
        } else if (proxy.type == ProxyType::SOCKS5) {
            if (!socks5Handshake(sock, host, port, proxy.username, proxy.password)) {
                close(sock);
                resp.error_message = "SOCKS5 handshake failed";
                return resp;
            }
        }
    }

    SSL* ssl = nullptr;
    std::string requestStr = buildHttpRequest(req);
    if (requestStr.empty()) {
        close(sock);
        resp.error_message = "Failed to build HTTP request";
        return resp;
    }

    if (use_ssl) {
        ssl = SSL_new(ctx);
        if (!ssl) {
            close(sock);
            resp.error_message = "SSL_new failed";
            return resp;
        }

        SSL_set_fd(ssl, sock);

        if (!SSL_set_tlsext_host_name(ssl, host.c_str())) {
            SSL_free(ssl);
            close(sock);
            resp.error_message = "SSL SNI setup failed";
            return resp;
        }

        int ssl_ret = SSL_connect(ssl);
        if (ssl_ret != 1) {
            int err = SSL_get_error(ssl, ssl_ret);
            std::ostringstream oss;
            oss << "SSL_connect failed: error=" << err
                << " " << ERR_error_string(ERR_get_error(), nullptr);
            resp.error_message = oss.str();
            SSL_free(ssl);
            close(sock);
            return resp;
        }

        int sent = SSL_write(ssl, requestStr.c_str(), static_cast<int>(requestStr.size()));
        if (sent <= 0) {
            int err = SSL_get_error(ssl, sent);
            std::ostringstream oss;
            oss << "SSL_write failed: error=" << err;
            resp.error_message = oss.str();
            SSL_shutdown(ssl);
            SSL_free(ssl);
            close(sock);
            return resp;
        }

        std::string rawResponse;
        char buf[8192];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        while (true) {
            int remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining_ms <= 0) break;

            int sock_fd = SSL_get_fd(ssl);
            struct pollfd pfd;
            pfd.fd = sock_fd;
            pfd.events = POLLIN;
            int pret = poll(&pfd, 1, remaining_ms);
            if (pret < 0) break;
            if (pret == 0) break;

            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n > 0) {
                rawResponse.append(buf, static_cast<size_t>(n));
            } else {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                break;
            }
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sock);

        resp = parseHttpResponse(rawResponse);
    } else {
        if (!sendAll(sock, requestStr.c_str(), requestStr.size())) {
            close(sock);
            resp.error_message = "Failed to send HTTP request";
            return resp;
        }

        std::string rawResponse;
        if (!recvAll(sock, rawResponse, timeout)) {
            close(sock);
            resp.error_message = "Failed to receive HTTP response";
            return resp;
        }
        close(sock);

        resp = parseHttpResponse(rawResponse);
    }

    return resp;
}

RateLimitInfo parseRateLimitHeaders(const Response& resp) {
    RateLimitInfo info;
    auto it = resp.headers.find("X-RateLimit-Limit");
    if (it != resp.headers.end()) {
        try { info.limit = std::stoi(it->second); } catch (...) {}
    }
    it = resp.headers.find("X-RateLimit-Remaining");
    if (it != resp.headers.end()) {
        try { info.remaining = std::stoi(it->second); } catch (...) {}
    }
    it = resp.headers.find("X-RateLimit-Reset");
    if (it != resp.headers.end()) {
        try { info.reset_ms = std::stoll(it->second) * 1000; } catch (...) {}
    }
    return info;
}

std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase
                    << static_cast<int>(c)
                    << std::nouppercase;
        }
    }
    return escaped.str();
}

}} // namespace matrixcli::http

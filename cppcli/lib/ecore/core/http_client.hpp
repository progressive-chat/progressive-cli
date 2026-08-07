// src/core/http_client.hpp — libcurl-based HTTP client for progressive-desktop.
//
// Replaces the JNI-based http_client.cpp in progressive_native (which
// delegates TLS to Android's javax.net.ssl.SSLSocket via tls_bridge.cpp).
// On desktop we use libcurl directly — supports TLS, SOCKS5 (Tor/I2P),
// HTTP proxy, redirects, timeouts.
//
// All functions are synchronous. The SyncEngine runs them on a worker
// thread; UI thread never blocks on HTTP.

#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace progressive::desktop {

struct HttpRequest {
    std::string method = "GET";                // GET / POST / PUT / DELETE
    std::string url;                            // Full URL including https://
    std::string body;                           // Request body (JSON)
    std::unordered_map<std::string, std::string> headers;
    int timeoutMs = 30000;                      // Connect + read timeout
    bool followRedirects = true;
};

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    bool success = false;
    std::string errorMessage;

    bool isOk() const { return statusCode >= 200 && statusCode < 300; }
    bool isClientError() const { return statusCode >= 400 && statusCode < 500; }
    bool isServerError() const { return statusCode >= 500; }
};

// Proxy configuration. SOCKS5 supports Tor (127.0.0.1:9050) and I2P.
struct ProxyConfig {
    bool enabled = false;
    std::string host;                           // "127.0.0.1"
    int port = 0;                               // 9050 for Tor, 4444 for I2P HTTP
    enum class Type { Http, Socks5, Socks5Hostname } type = Type::Socks5Hostname;
    std::string username;
    std::string password;
};

// Initialize libcurl globally. Call once at program start.
void httpInit();

// Cleanup libcurl globally. Call once at program exit.
void httpCleanup();

// Set the global proxy (applies to all subsequent requests).
// Pass ProxyConfig{.enabled=false} to disable.
void setGlobalProxy(const ProxyConfig& cfg);

// Execute a single HTTP request. Synchronous.
HttpResponse httpExecute(const HttpRequest& req);

// HTTP request log entry (for network log viewer in UI).
struct HttpLogEntry {
    std::string method;
    std::string url;
    int statusCode = 0;
    size_t responseBytes = 0;
    int64_t elapsedMs = 0;
    std::string error;
};

// Return a snapshot of the last N HTTP requests (ring buffer, max 500).
std::vector<HttpLogEntry> getHttpLog();

// Clear the HTTP request log buffer.
void clearHttpLog();

// Convenience helpers
HttpResponse httpGet(const std::string& url,
                     const std::unordered_map<std::string, std::string>& headers = {},
                     int timeoutMs = 30000);

HttpResponse httpPost(const std::string& url,
                      const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers = {},
                      int timeoutMs = 30000);

HttpResponse httpPut(const std::string& url,
                     const std::string& body,
                     const std::unordered_map<std::string, std::string>& headers = {},
                     int timeoutMs = 30000);

HttpResponse httpDelete(const std::string& url,
                        const std::unordered_map<std::string, std::string>& headers = {},
                        int timeoutMs = 30000);

} // namespace progressive::desktop

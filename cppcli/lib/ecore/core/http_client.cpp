// src/core/http_client.cpp — libcurl implementation.
//
// Why libcurl and not the progressive_native http_client.cpp?
// The upstream module delegates TLS to Android's javax.net.ssl.SSLSocket
// via tls_bridge.cpp (JNI). On desktop there is no JVM — we use libcurl
// which handles TLS (OpenSSL), proxy, SOCKS5 (Tor/I2P), redirects.

#include "http_client.hpp"
#include "core/version.h"

#include <curl/curl.h>
#include <cstring>
#include <mutex>
#include <sstream>
#include <deque>
#include <chrono>

namespace progressive::desktop {

// Global proxy config (set via setGlobalProxy, read in each request).
static ProxyConfig g_proxy;
static std::mutex g_proxy_mutex;

static bool g_initialized = false;

// HTTP request log ring buffer (max 500 entries).
static constexpr size_t kMaxLogEntries = 500;
static std::deque<HttpLogEntry> g_log;
static std::mutex g_log_mutex;

static void logHttpRequest(const std::string& method, const std::string& url,
                           int statusCode, size_t responseBytes, int64_t elapsedMs,
                           const std::string& error) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::string shortUrl = url;
    auto q = shortUrl.find('?');
    if (q != std::string::npos) shortUrl = shortUrl.substr(0, q);
    g_log.push_back({method, shortUrl, statusCode, responseBytes, elapsedMs, error});
    while (g_log.size() > kMaxLogEntries) g_log.pop_front();
}

std::vector<HttpLogEntry> getHttpLog() {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    return std::vector<HttpLogEntry>(g_log.begin(), g_log.end());
}

void clearHttpLog() {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log.clear();
}

namespace {
size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// libcurl header callback — store as "key: value" (lowercased key)
size_t headerCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* hdrs = static_cast<std::unordered_map<std::string, std::string>*>(userdata);
    size_t total = size * nitems;
    std::string line(buffer, total);
    // Strip trailing \r\n
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    auto colon = line.find(':');
    if (colon == std::string::npos) return total;
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    // trim leading space from val
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
    // lowercase key
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    (*hdrs)[key] = val;
    return total;
}

long proxyTypeCode(ProxyConfig::Type t) {
    switch (t) {
        case ProxyConfig::Type::Http:            return CURLPROXY_HTTP;
        case ProxyConfig::Type::Socks5:          return CURLPROXY_SOCKS5;
        case ProxyConfig::Type::Socks5Hostname:  return CURLPROXY_SOCKS5_HOSTNAME;
    }
    return CURLPROXY_HTTP;
}

} // namespace

void httpInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        std::lock_guard<std::mutex> lk(g_proxy_mutex);
        g_initialized = true;
    });
}

void httpCleanup() {
    std::lock_guard<std::mutex> lk(g_proxy_mutex);
    if (g_initialized) {
        curl_global_cleanup();
        g_initialized = false;
    }
}

void setGlobalProxy(const ProxyConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_proxy_mutex);
    g_proxy = cfg;
}

HttpResponse httpExecute(const HttpRequest& req) {
    auto t0 = std::chrono::steady_clock::now();
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.errorMessage = "curl_easy_init failed";
        logHttpRequest(req.method, req.url, 0, 0, 0, resp.errorMessage);
        return resp;
    }

    std::string bodyBuf;
    std::unordered_map<std::string, std::string> hdrBuf;

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bodyBuf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdrBuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req.timeoutMs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "progressive-desktop/" PROGRESSIVE_DESKTOP_VERSION);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Method + body
    if (req.method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
        }
    } else if (req.method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
        }
    } else if (req.method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    }

    // Headers
    struct curl_slist* hdrList = nullptr;
    for (const auto& [k, v] : req.headers) {
        std::string entry = k + ": " + v;
        hdrList = curl_slist_append(hdrList, entry.c_str());
    }
    if (hdrList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrList);

    // Proxy
    {
        std::lock_guard<std::mutex> lk(g_proxy_mutex);
        if (g_proxy.enabled && !g_proxy.host.empty()) {
            std::string proxyUrl = g_proxy.host + ":" + std::to_string(g_proxy.port);
            curl_easy_setopt(curl, CURLOPT_PROXY, proxyUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxyTypeCode(g_proxy.type));
            if (!g_proxy.username.empty()) {
                std::string creds = g_proxy.username + ":" + g_proxy.password;
                curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, creds.c_str());
            }
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    auto t1 = std::chrono::steady_clock::now();
    int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (rc != CURLE_OK) {
        resp.errorMessage = "curl: " + std::string(curl_easy_strerror(rc));
        logHttpRequest(req.method, req.url, 0, 0, elapsed, resp.errorMessage);
        if (hdrList) curl_slist_free_all(hdrList);
        curl_easy_cleanup(curl);
        return resp;
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    resp.statusCode = static_cast<int>(code);
    resp.body = std::move(bodyBuf);
    resp.headers = std::move(hdrBuf);
    resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);

    logHttpRequest(req.method, req.url, resp.statusCode, resp.body.size(), elapsed, "");

    if (hdrList) curl_slist_free_all(hdrList);
    curl_easy_cleanup(curl);
    return resp;
}

HttpResponse httpGet(const std::string& url,
                     const std::unordered_map<std::string, std::string>& headers,
                     int timeoutMs) {
    return httpExecute({"GET", url, "", headers, timeoutMs, true});
}

HttpResponse httpPost(const std::string& url, const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers,
                      int timeoutMs) {
    return httpExecute({"POST", url, body, headers, timeoutMs, true});
}

HttpResponse httpPut(const std::string& url, const std::string& body,
                     const std::unordered_map<std::string, std::string>& headers,
                     int timeoutMs) {
    return httpExecute({"PUT", url, body, headers, timeoutMs, true});
}

HttpResponse httpDelete(const std::string& url,
                        const std::unordered_map<std::string, std::string>& headers,
                        int timeoutMs) {
    return httpExecute({"DELETE", url, "", headers, timeoutMs, true});
}

} // namespace progressive::desktop

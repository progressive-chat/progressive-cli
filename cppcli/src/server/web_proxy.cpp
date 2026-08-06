#include "web_proxy.hpp"
#include "../../lib/util/logger.hpp"
#include "../../lib/http/http.hpp"

#include <sstream>
#include <nlohmann/json.hpp>

namespace matrixcli { namespace server {

WebProxyHandler::WebProxyHandler(const std::string& homeserver_url)
    : _homeserver(homeserver_url) {
    if (_homeserver.back() == '/') _homeserver.pop_back();
}

api::Response WebProxyHandler::handleHealth(const api::Request&) {
    api::Response resp;
    resp.content_type = "application/json";
    resp.body = R"({"status":"ok","mode":"web-proxy","homeserver":")" + _homeserver + "\"}";
    return resp;
}

api::Response WebProxyHandler::handleProxy(const api::Request& req) {
    api::Response resp;
    resp.content_type = "application/json";

    // Extract token from query, Authorization header, or body
    std::string token;
    auto tok = req.params.find("access_token");
    if (tok != req.params.end()) token = tok->second;

    auto auth = req.headers.find("authorization");
    if (token.empty() && auth != req.headers.end()) {
        std::string ah = auth->second;
        if (ah.starts_with("Bearer ")) token = ah.substr(7);
    }

    // Try to parse token from JSON body
    if (token.empty() && !req.body.empty()) {
        try {
            auto j = nlohmann::json::parse(req.body);
            if (j.contains("access_token")) token = j["access_token"];
        } catch (...) {}
    }

    if (token.empty()) {
        resp.status = 401;
        resp.body = R"({"error":"No access_token. Login at your Matrix homeserver and pass ?access_token= or Authorization: Bearer"})";
        return resp;
    }

    // Forward to homeserver
    try {
        http::Client client;
        std::string target_path = req.path;
        // Remove /api/proxy prefix if present
        if (target_path.starts_with("/api/proxy")) {
            target_path = target_path.substr(10); // strlen("/api/proxy")
        }
        if (target_path.empty()) target_path = "/";

        std::string url = _homeserver + target_path;

        // Copy query params except access_token
        std::string qs;
        for (auto& [k, v] : req.params) {
            if (k == "access_token") continue;
            if (!qs.empty()) qs += "&";
            qs += k + "=" + v;
        }
        if (!qs.empty()) url += "?" + qs;

        // Forward with token
        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + token},
            {"Content-Type", req.headers.count("content-type") ?
                req.headers.at("content-type") : "application/json"}
        };

        http::Response proxyResp;
        if (req.method == "GET") {
            proxyResp = client.get(url, headers);
        } else if (req.method == "POST") {
            proxyResp = client.post(url, req.body, headers);
        } else if (req.method == "PUT") {
            proxyResp = client.put(url, req.body, headers);
        } else {
            resp.status = 405;
            resp.body = R"({"error":"method not supported"})";
            return resp;
        }

        resp.status = proxyResp.status_code;
        resp.body = proxyResp.body;
        resp.content_type = proxyResp.headers.count("content-type") ?
            proxyResp.headers.at("content-type") : "application/json";
    } catch (const std::exception& e) {
        resp.status = 502;
        resp.body = std::string(R"({"error":"proxy error: ")") + e.what() + "\"}";
    }

    return resp;
}

void WebProxyHandler::registerRoutes(api::Router& router) {
    router.get("/api/health", [this](const api::Request& req) {
        return handleHealth(req);
    });
    router.get("/api/proxy/*", [this](const api::Request& req) {
        return handleProxy(req);
    });
    router.post("/api/proxy/*", [this](const api::Request& req) {
        return handleProxy(req);
    });
    router.put("/api/proxy/*", [this](const api::Request& req) {
        return handleProxy(req);
    });
}

}} // namespace matrixcli::server

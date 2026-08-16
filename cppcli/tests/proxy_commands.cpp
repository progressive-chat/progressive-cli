// src/proxy_commands.cpp — Tor/I2P/SOCKS5 proxy configuration for the CLI.
//
// Wraps the progressive-core global proxy (setGlobalProxy/getGlobalProxy,
// src/core/http_client.{hpp,cpp}): `proxy on --host H --port P [--type ...]`,
// `proxy off`, `proxy status [--json]`. The setting persists in config.json
// and is auto-applied at every process start (applyProxyFromConfig, called
// from main()). Registered via CommandRegistry.
//
// SECURITY PROPERTY: the default type is socks5h (Socks5Hostname) — the proxy
// resolves DNS itself, so the client NEVER leaks the target hostname via a
// local DNS query. All core traffic (matrix_client, decryptor, media, LLM)
// flows through progressive::desktop::httpExecute, which consults the global
// proxy on every request.
#include "commands.hpp"
#include "config.hpp"
#include "core/http_client.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace matrixcli;

namespace {

// Parse a --type value into a core ProxyConfig::Type. Accepts the curl-ish
// spellings: socks5h (default), socks5, http.
progressive::desktop::ProxyConfig::Type parseProxyType(const std::string& s,
                                                       bool* ok) {
    *ok = true;
    if (s == "socks5h") return progressive::desktop::ProxyConfig::Type::Socks5Hostname;
    if (s == "socks5")  return progressive::desktop::ProxyConfig::Type::Socks5;
    if (s == "http")    return progressive::desktop::ProxyConfig::Type::Http;
    *ok = false;
    return progressive::desktop::ProxyConfig::Type::Socks5Hostname;
}

std::string proxyTypeName(progressive::desktop::ProxyConfig::Type t) {
    switch (t) {
        case progressive::desktop::ProxyConfig::Type::Http:           return "http";
        case progressive::desktop::ProxyConfig::Type::Socks5:         return "socks5";
        case progressive::desktop::ProxyConfig::Type::Socks5Hostname: return "socks5h";
    }
    return "socks5h";
}

// Minimal ANSI color helpers. Colors are only emitted when stdout is a TTY,
// so piped output (scripts, --json) stays plain.
// Levels: important = bright bold, normal = cyan, unimportant = dim.
bool useColor() {
    static const bool tty = ::isatty(::fileno(stdout)) != 0;
    return tty;
}

std::string paint(const char* code, const std::string& s) {
    return useColor() ? std::string(code) + s + "\033[0m" : s;
}

} // namespace

// Dump the on-disk config.json verbatim (best-effort), white-highlighting
// the proxy_enabled line (the main reason for the current state).
void dumpConfigFile() {
    std::ifstream in("config.json");
    if (!in) return;
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    std::cout << paint("\033[90m", "config.json:") << std::endl;
    size_t start = 0;
    while (start <= contents.size()) {
        size_t nl = contents.find('\n', start);
        std::string line = contents.substr(start,
            nl == std::string::npos ? std::string::npos : nl - start);
        if (line.find("proxy_enabled") != std::string::npos)
            std::cout << paint("\033[1;37m", line) << std::endl;
        else
            std::cout << paint("\033[90m", line) << std::endl;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// Apply the persisted proxy config (config.json) to the core's global proxy.
// Called once at startup from main(). No-op when nothing is configured.
void applyProxyFromConfig() {
    Config::instance().load("config.json");
    progressive::desktop::ProxyConfig cfg;
    if (Config::instance().get("proxy_enabled") == "true") {
        cfg.enabled = true;
        cfg.host = Config::instance().get("proxy_host");
        cfg.port = std::stoi(Config::instance().get("proxy_port").empty()
                                 ? "0" : Config::instance().get("proxy_port"));
        bool ok = true;
        cfg.type = parseProxyType(Config::instance().get("proxy_type"), &ok);
        if (!ok) cfg.type = progressive::desktop::ProxyConfig::Type::Socks5Hostname;
        cfg.username = Config::instance().get("proxy_user");
        cfg.password = Config::instance().get("proxy_pass");
    }
    progressive::desktop::setGlobalProxy(cfg);
}

int cmdProxy(const cli::Args& args) {
    std::string sub = args.positional.empty() ? "status" : args.positional[0];
    bool json_out = args.options.count("json");

    if (sub == "on") {
        auto host_it = args.options.find("host");
        auto port_it = args.options.find("port");
        if (host_it == args.options.end() || port_it == args.options.end()) {
            std::cerr << "proxy on: --host and --port required (e.g. --host 127.0.0.1 --port 9050)"
                      << std::endl;
            return 1;
        }
        int port = 0;
        try { port = std::stoi(port_it->second); } catch (...) {}
        if (port <= 0 || port > 65535) {
            std::cerr << "proxy on: invalid --port " << port_it->second << std::endl;
            return 1;
        }

        bool ok = true;
        auto type_it = args.options.find("type");
        auto type = parseProxyType(type_it == args.options.end() ? "socks5h" : type_it->second, &ok);
        if (!ok) {
            std::cerr << "proxy on: --type must be socks5h|socks5|http" << std::endl;
            return 1;
        }

        progressive::desktop::ProxyConfig cfg;
        cfg.enabled = true;
        cfg.host = host_it->second;
        cfg.port = port;
        cfg.type = type;
        auto user_it = args.options.find("user");
        if (user_it != args.options.end()) cfg.username = user_it->second;
        auto pass_it = args.options.find("pass");
        if (pass_it != args.options.end()) cfg.password = pass_it->second;
        progressive::desktop::setGlobalProxy(cfg);

        // Persist for the next process.
        Config::instance().set("proxy_enabled", "true");
        Config::instance().set("proxy_host", cfg.host);
        Config::instance().set("proxy_port", std::to_string(cfg.port));
        Config::instance().set("proxy_type", proxyTypeName(cfg.type));
        Config::instance().set("proxy_user", cfg.username);
        Config::instance().set("proxy_pass", cfg.password);
        Config::instance().save();

        std::cout << "proxy enabled: " << proxyTypeName(cfg.type) << "://" << cfg.host
                  << ":" << cfg.port << std::endl;
        return 0;
    }

    if (sub == "off") {
        progressive::desktop::setGlobalProxy(progressive::desktop::ProxyConfig{});
        Config::instance().set("proxy_enabled", "false");
        Config::instance().save();
        std::cout << "proxy disabled — direct connections" << std::endl;
        return 0;
    }

    // status (default)
    auto cfg = progressive::desktop::getGlobalProxy();
    if (json_out) {
        nlohmann::json j;
        j["enabled"] = cfg.enabled;
        j["host"] = cfg.host;
        j["port"] = cfg.port;
        j["type"] = proxyTypeName(cfg.type);
        j["auth"] = !cfg.username.empty();
        std::cout << j.dump() << std::endl;
    } else {
        if (!cfg.enabled) {
            std::cout << paint("\033[1;31m",
                "proxy: disabled (direct connections) — to enable: proxy on --host H --port P")
                      << std::endl;
            dumpConfigFile();
        } else {
            std::string line = "proxy: " + proxyTypeName(cfg.type) + "://" + cfg.host
                             + ":" + std::to_string(cfg.port);
            if (!cfg.username.empty()) line += " (auth)";
            std::cout << paint("\033[1;37m", line) << std::endl;
            dumpConfigFile();
        }
    }
    return 0;
}

void registerProxyCommands() {
    CommandRegistry::instance().registerCli("proxy", cmdProxy,
        "proxy on|off|status [--host H --port P --type socks5h|socks5|http [--user U --pass P]] [--json]");
}

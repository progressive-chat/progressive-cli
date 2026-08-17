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

// Well-known proxies, addressable as `proxy on <name|N>`. Stored in
// config.json under "proxy_presets" (editable by hand); the two defaults
// below are seeded into the config on the first run. The currently chosen
// preset is kept in "proxy_active" — a single source of truth, so the same
// proxy never appears twice in config.json.
struct ProxyPreset {
    std::string name;
    std::string type;
    std::string host;
    int port;
    std::string user;
    std::string pass;
};

nlohmann::json defaultPresetsJson() {
    return nlohmann::json::array({
        {{"name", "tor"}, {"type", "socks5h"}, {"host", "127.0.0.1"}, {"port", 9050}},
        {{"name", "i2p"}, {"type", "http"},    {"host", "127.0.0.1"}, {"port", 4444}},
    });
}

std::vector<ProxyPreset> loadPresets() {
    std::vector<ProxyPreset> out;
    auto arr = Config::instance().getRaw("proxy_presets");
    if (!arr.is_array()) return out;
    for (auto& e : arr) {
        if (!e.is_object()) continue;
        ProxyPreset p;
        p.name = e.value("name", std::string("preset"));
        p.type = e.value("type", std::string("socks5h"));
        p.host = e.value("host", std::string());
        p.port = e.value("port", 0);
        p.user = e.value("user", std::string());
        p.pass = e.value("pass", std::string());
        if (p.host.empty() || p.port <= 0 || p.port > 65535) continue;
        out.push_back(p);
    }
    return out;
}

nlohmann::json presetToJson(const ProxyPreset& p) {
    nlohmann::json j = {{"name", p.name}, {"type", p.type},
                        {"host", p.host}, {"port", p.port}};
    if (!p.user.empty()) j["user"] = p.user;
    if (!p.pass.empty()) j["pass"] = p.pass;
    return j;
}

void savePresets(const std::vector<ProxyPreset>& presets) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : presets) arr.push_back(presetToJson(p));
    Config::instance().setRaw("proxy_presets", arr);
}

std::string presetsSummary() {
    auto presets = loadPresets();
    std::string s;
    for (size_t i = 0; i < presets.size(); ++i) {
        if (i) s += " · ";
        s += std::to_string(i + 1) + " " + presets[i].name + ": "
           + presets[i].type + "://" + presets[i].host + ":"
           + std::to_string(presets[i].port);
    }
    return s;
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
// the proxy_active line (the main reason for the current state).
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
        if (line.find("proxy_active") != std::string::npos)
            std::cout << paint("\033[1;37m", line) << std::endl;
        else
            std::cout << paint("\033[90m", line) << std::endl;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// Apply the persisted proxy config (config.json) to the core's global proxy.
// Called once at startup from main(). No-op when nothing is configured.
// One-time migration: the old flat proxy_* keys move into the presets model
// ("proxy_active" + "proxy_presets") and the flat keys are removed, so the
// same proxy is never stored twice.
void migrateFlatProxyKeys() {
    bool enabled = Config::instance().get("proxy_enabled") == "true";
    std::string host = Config::instance().get("proxy_host");
    std::string port_s = Config::instance().get("proxy_port");
    int port = 0;
    try { port = std::stoi(port_s.empty() ? "0" : port_s); } catch (...) {}
    std::string type = Config::instance().get("proxy_type");
    std::string user = Config::instance().get("proxy_user");
    std::string pass = Config::instance().get("proxy_pass");
    auto presets = loadPresets();
    std::string active = Config::instance().get("proxy_active");
    if (enabled && !host.empty() && port > 0) {
        bool found = false;
        for (const auto& p : presets) {
            if (p.host == host && p.port == port && (type.empty() || p.type == type)) {
                active = p.name;
                found = true;
                break;
            }
        }
        if (!found) {
            std::string name = "custom";
            int n = 1;
            while (std::any_of(presets.begin(), presets.end(),
                               [&](const ProxyPreset& p) { return p.name == name; }))
                name = "custom-" + std::to_string(++n);
            presets.push_back({name, type.empty() ? "socks5h" : type, host, port, user, pass});
            active = name;
        }
    }
    savePresets(presets);
    Config::instance().set("proxy_active", active);
    Config::instance().erase("proxy_enabled");
    Config::instance().erase("proxy_host");
    Config::instance().erase("proxy_port");
    Config::instance().erase("proxy_type");
    Config::instance().erase("proxy_user");
    Config::instance().erase("proxy_pass");
    Config::instance().save();
}

// Fill *out from the active preset; returns false when no proxy is active.
bool activeProxyConfig(progressive::desktop::ProxyConfig* out) {
    std::string active = Config::instance().get("proxy_active");
    if (active.empty()) return false;
    for (const auto& p : loadPresets()) {
        if (p.name == active) {
            out->enabled = true;
            out->host = p.host;
            out->port = p.port;
            bool ok = true;
            out->type = parseProxyType(p.type, &ok);
            if (!ok) out->type = progressive::desktop::ProxyConfig::Type::Socks5Hostname;
            out->username = p.user;
            out->password = p.pass;
            return true;
        }
    }
    return false;
}

// Make a proxy the active one, storing it in config.json as a preset (a
// matching entry is reused, otherwise a "custom" preset is added) — this is
// the only writer of the proxy state, so no duplicates can appear.
void persistActiveProxy(const progressive::desktop::ProxyConfig& cfg) {
    if (!Config::instance().getRaw("proxy_presets").is_array()) {
        Config::instance().setRaw("proxy_presets", defaultPresetsJson());
    }
    if (!cfg.enabled) {
        Config::instance().set("proxy_active", "");
        Config::instance().save();
        return;
    }
    auto presets = loadPresets();
    std::string active;
    for (const auto& p : presets) {
        if (p.host == cfg.host && p.port == cfg.port
            && p.type == proxyTypeName(cfg.type)) {
            active = p.name;
            break;
        }
    }
    if (active.empty()) {
        std::string name = "custom";
        int n = 1;
        while (std::any_of(presets.begin(), presets.end(),
                           [&](const ProxyPreset& p) { return p.name == name; }))
            name = "custom-" + std::to_string(++n);
        presets.push_back({name, proxyTypeName(cfg.type), cfg.host, cfg.port,
                           cfg.username, cfg.password});
        active = name;
    }
    savePresets(presets);
    Config::instance().set("proxy_active", active);
    Config::instance().save();
}

void applyProxyFromConfig() {
    Config::instance().load("config.json");

    // One-time migration of the old flat proxy_* keys (removes duplicates).
    if (!Config::instance().get("proxy_enabled").empty()
        || !Config::instance().get("proxy_host").empty()) {
        migrateFlatProxyKeys();
    }

    // Seed the default presets once — config.json is the source of truth,
    // the user can edit them by hand there.
    if (!Config::instance().getRaw("proxy_presets").is_array()) {
        Config::instance().setRaw("proxy_presets", defaultPresetsJson());
        Config::instance().save();
    }

    progressive::desktop::ProxyConfig cfg;
    activeProxyConfig(&cfg);
    progressive::desktop::setGlobalProxy(cfg);
}

int cmdProxy(const cli::Args& args) {
    std::string sub = args.positional.empty() ? "status" : args.positional[0];
    bool json_out = args.options.count("json");

    if (sub == "presets") {
        auto presets = loadPresets();
        if (presets.empty()) {
            std::cout << "no presets in config.json — add a \"proxy_presets\" array"
                      << " (or delete the file to reseed the defaults)" << std::endl;
            return 0;
        }
        for (size_t i = 0; i < presets.size(); ++i) {
            std::cout << (i + 1) << ": " << presets[i].name << " — "
                      << presets[i].type << "://" << presets[i].host << ":"
                      << presets[i].port << "  (proxy on " << (i + 1) << ")"
                      << std::endl;
        }
        return 0;
    }

    if (sub == "on") {
        bool custom = args.options.count("host") || args.options.count("port");
        if (!custom) {
            // Preset path: `proxy on <name|N>` (a bare `proxy on` re-enables
            // the last active preset).
            std::string ref = args.positional.size() > 1
                ? args.positional[1] : Config::instance().get("proxy_active");
            if (ref.empty()) {
                std::cerr << "proxy on: no preset given — run 'proxy presets' "
                          << "(or --host/--port for a custom proxy)" << std::endl;
                return 1;
            }
            auto presets = loadPresets();
            int idx = -1;
            try {
                int n = std::stoi(ref);
                if (n >= 1 && static_cast<size_t>(n) <= presets.size()) idx = n - 1;
            } catch (...) {}
            if (idx < 0) {
                for (size_t i = 0; i < presets.size(); ++i) {
                    if (presets[i].name == ref) { idx = static_cast<int>(i); break; }
                }
            }
            if (idx < 0) {
                std::cerr << "proxy on: unknown preset '" << ref
                          << "' — run 'proxy presets' (presets live in config.json)"
                          << std::endl;
                return 1;
            }
            const ProxyPreset& p = presets[idx];
            progressive::desktop::ProxyConfig cfg;
            cfg.enabled = true;
            cfg.host = p.host;
            cfg.port = p.port;
            bool ok = true;
            cfg.type = parseProxyType(p.type, &ok);
            cfg.username = p.user;
            cfg.password = p.pass;
            progressive::desktop::setGlobalProxy(cfg);
            persistActiveProxy(cfg);
            std::cout << "proxy enabled: " << proxyTypeName(cfg.type) << "://" << cfg.host
                      << ":" << cfg.port << " (" << p.name << " preset)" << std::endl;
            return 0;
        }

        auto host_it = args.options.find("host");
        auto port_it = args.options.find("port");
        if (host_it == args.options.end() || port_it == args.options.end()) {
            std::cerr << "proxy on: --host and --port required (e.g. --host 127.0.0.1 --port 9050)"
                      << ", or a preset — run 'proxy presets'" << std::endl;
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
        auto type = parseProxyType(type_it == args.options.end() ? "socks5h"
                                                                 : type_it->second, &ok);
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
        persistActiveProxy(cfg);

        std::cout << "proxy enabled: " << proxyTypeName(cfg.type) << "://" << cfg.host
                  << ":" << cfg.port;
        if (!cfg.username.empty()) std::cout << " (auth)";
        std::cout << std::endl;
        return 0;
    }

    if (sub == "off") {
        progressive::desktop::setGlobalProxy(progressive::desktop::ProxyConfig{});
        persistActiveProxy(progressive::desktop::ProxyConfig{});
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
                "proxy: disabled (direct connections) — to enable: proxy on <name|N>")
                      << std::endl;
            std::cout << paint("\033[90m",
                "  presets: " + presetsSummary() + " — 'proxy on N', or --host/--port")
                      << std::endl;
            dumpConfigFile();
        } else {
            std::string line = "proxy: enabled (proxied connections) — to disable: proxy off";
            std::cout << paint("\033[1;32m", line) << std::endl;
            std::cout << paint("\033[90m",
                "  switch: proxy on <name|N> — presets: " + presetsSummary())
                      << std::endl;
            dumpConfigFile();
        }
    }
    return 0;
}

void registerProxyCommands() {
    CommandRegistry::instance().registerCli("proxy", cmdProxy,
        "proxy on|off|status|presets [<name|N>|--host H --port P --type socks5h|socks5|http [--user U --pass P]] [--json]");
}

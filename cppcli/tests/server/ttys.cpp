#include "ttys.hpp"
#include "../../lib/util/logger.hpp"
#include "../../lib/util/string_utils.hpp"
#include "../proxy_commands.hpp"
#include "../config.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <regex>
#include <sstream>
#include <unordered_set>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace matrixcli { namespace server {

// Apply the proxy configured via `progressive-cli proxy on` (config.json) to
// a session's matrix client. lib/http (what matrix::Client uses) only reads
// the env proxy (all_proxy/https_proxy) on its own, NOT the core's global
// proxy, so the configured one is set explicitly here.
static void applyConfiguredProxy(matrix::Client& client) {
    try { Config::instance().load("config.json"); } catch (...) {}
    progressive::desktop::ProxyConfig active;
    if (!activeProxyConfig(&active)) return;
    http::ProxyConfig p;
    p.host = active.host;
    p.port = active.port;
    p.type = (active.type == progressive::desktop::ProxyConfig::Type::Http)
                 ? http::ProxyType::HTTP : http::ProxyType::SOCKS5;
    p.username = active.username;
    p.password = active.password;
    client.setProxy(p);
}

// A per-request "proxy" spec: "socks5://[user:pass@]host:port",
// "http://host:port" or "off" (= direct; the env-var fallback inside
// lib/http still applies when nothing else is set).
static http::ProxyConfig parseProxySpec(const std::string& spec) {
    http::ProxyConfig p;
    if (spec.empty() || spec == "off" || spec == "direct" || spec == "none")
        return p;
    static const std::regex re(
        R"(^(socks5h?|http)://(?:([^:@]+):([^@]+)@)?([^:/@]+):(\d+)/?$)");
    std::smatch m;
    if (!std::regex_match(spec, m, re)) return p;
    p.type = m[1].str() == "http" ? http::ProxyType::HTTP
                                  : http::ProxyType::SOCKS5;
    try { p.port = std::stoi(m[5].str()); } catch (...) { return {}; }
    p.host = m[4].str();
    if (m[2].matched) { p.username = m[2].str(); p.password = m[3].str(); }
    return p;
}

// The per-session proxy override: present in the request body -> it wins
// ("off" clears); absent -> fall back to the server-wide configured proxy.
static void applySessionProxy(matrix::Client& client, const json& body,
                              std::string& storedSpec) {
    auto it = body.find("proxy");
    if (it == body.end() || !it->is_string()) {
        applyConfiguredProxy(client);
        return;
    }
    storedSpec = it->get<std::string>();
    client.setProxy(parseProxySpec(storedSpec));
}

TtysApi::~TtysApi() {
    std::lock_guard<std::mutex> lock(_mu);
    for (auto& [id, s] : _sessions) {
        s->wantsAutoSync = false;
        if (s->syncThread.joinable()) s->syncThread.join();
        if (s->client) s->client->stopSync();
    }
}

void TtysApi::registerRoutes(api::Router& router) {
    router.post("/api/ttys/register", [this](const api::Request& req) { return handleRegister(req); });
    router.post("/api/ttys/session", [this](const api::Request& req) { return handleSession(req); });
    router.post("/api/ttys/render",  [this](const api::Request& req) { return handleRender(req); });
    router.post("/api/ttys/input",   [this](const api::Request& req) { return handleInput(req); });
    router.post("/api/ttys/sync",    [this](const api::Request& req) { return handleSync(req); });
    router.post("/api/ttys/proxy",   [this](const api::Request& req) { return handleProxy(req); });
    router.get("/api/ttys/proxy",    [this](const api::Request& req) { return handleProxy(req); });
}

int TtysApi::activeSessionCount() {
    std::lock_guard<std::mutex> lock(_mu);
    return static_cast<int>(_sessions.size());
}

TtysSession* TtysApi::findSession(const std::string& id) {
    auto it = _sessions.find(id);
    return it != _sessions.end() ? it->second.get() : nullptr;
}

// Build a session (or return the existing one). The account fields come
// from the HTTP request — NEVER persisted to the server's home dir (the
// serve side stores nothing; 'ram' is the default and only storage).
// The caller must hold _mu.
TtysSession* TtysApi::createOrGetSession(const std::string& id,
                                         const json& account) {
    TtysSession* s = findSession(id);
    if (s) return s;

    std::string hs = account.value("homeserver", "");
    std::string tok = account.value("access_token", "");
    if (hs.empty() || tok.empty()) return nullptr;

    auto sess = std::make_unique<TtysSession>();
    sess->id = id;
    sess->homeserver = hs;
    sess->accessToken = tok;
    sess->userId = account.value("user_id", "");
    sess->deviceId = account.value("device_id", "");
    try { sess->dbi = std::make_unique<db::Database>(); } catch (...) {}
    if (!sess->dbi) return nullptr;
    std::string cachePath = _persistCachePath.empty()
                                ? ":memory:" : _persistCachePath;
    if (!sess->dbi->open(cachePath)) {
        util::Logger::instance().warn("ttys: cannot open cache " + cachePath);
        return nullptr;
    }
    sess->client = std::make_unique<matrix::Client>();
    sess->client->setHomeserverURL(hs);
    sess->client->setAccessToken(tok);
    sess->client->setUserId(sess->userId);
    sess->client->setDatabase(sess->dbi.get());
    applySessionProxy(*sess->client, account, sess->proxySpec);
    sess->st.db = sess->dbi.get();

    _sessions[id] = std::move(sess);
    return _sessions[id].get();
}

// Sync one pass over /sync, then re-stamp the UiState from the cache.
void TtysApi::runSyncPass(TtysSession& s) {
    if (!s.client || !s.dbi) return;
    if (s.syncing.exchange(true)) return;
    try {
        auto resp = s.client->syncOnce("", "", 5000);
        (void)resp;
        s.lastSyncMs = duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        s.syncError.clear();
    } catch (const std::exception& e) {
        s.syncError = e.what();
        util::Logger::instance().warn("ttys sync error: " + s.syncError);
    }
    s.syncing = false;
}

// Fill UiState.rooms etc. from the in-memory cache (re-stamp after any).
void TtysApi::ensureStateLoaded(TtysSession& s) {
    if (!s.dbi) return;
    UiState& st = s.st;
    st.rooms = s.dbi->listRooms();
    sortRoomsByActivity(st);
    if (st.accountLabel.empty()) {
        std::string uid = s.userId;
        if (!uid.empty() && uid[0] == '@') uid = uid.substr(1);
        st.accountLabel = uid.empty() ? "ram" : uid;
    }
    if (st.currentRoomId.empty() && !st.rooms.empty()) {
        loadRoomIntoStateImpl(st, st.rooms.front().value("room_id", ""));
    } else if (!st.currentRoomId.empty()) {
        loadRoomIntoStateImpl(st, st.currentRoomId);
    }
}

std::string TtysApi::renderFrame(TtysSession& s, int cols, int rows) {
    ensureStateLoaded(s);
    s.st.termW = cols > 0 ? cols : 0;
    s.st.termH = rows > 0 ? rows : 0;
    return drawFrameImpl(s.st);
}

bool TtysApi::authorized(const api::Request& req) {
    if (_token.empty()) return true;
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return false;
    return it->second.rfind("Bearer ", 0) == 0 &&
           it->second.substr(7) == _token;
}

// Register a NEW Matrix account on the request's homeserver and open the
// ttys session for it in one step — so a curl-only user needs no binary:
//   curl -X POST .../api/ttys/register -d '{"homeserver":"https://hs",
//        "username":"new","password":"pw"[,"reg_token":"T"]}'
// The fresh credentials are returned to the caller (the thin-client
// protocol carries the account in every request) and kept in RAM only.
api::Response TtysApi::handleRegister(const api::Request& req) {
    if (!authorized(req)) {
        return {401, "application/json", R"({"error":"unauthorized"})"};
    }
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
        return {400, "application/json", R"({"error":"bad JSON"})"};
    }
    std::string hs = body.value("homeserver", "");
    std::string username = body.value("username", "");
    std::string password = body.value("password", "");
    std::string regToken = body.value("reg_token", "");
    if (hs.empty() || username.empty() || password.empty()) {
        return {400, "application/json",
                R"({"error":"homeserver, username and password required"})"};
    }
    // Optional per-request proxy for BOTH the registration call and the
    // session that follows ("socks5://h:p" / "http://h:p" / "off").
    std::string proxySpec;
    {
        auto it = body.find("proxy");
        if (it != body.end() && it->is_string()) proxySpec = it->get<std::string>();
    }
    // Registration takes the LOCALPART: strip @ and :server when the
    // caller passed a full Matrix ID.
    if (!username.empty() && username[0] == '@') {
        auto colon = username.find(':');
        username = colon != std::string::npos
                       ? username.substr(1, colon - 1) : username.substr(1);
    }
    matrix::Credentials creds;
    try {
        matrix::Client client;
        client.setHomeserverURL(hs);
        if (!proxySpec.empty() || body.contains("proxy"))
            client.setProxy(parseProxySpec(proxySpec));   // "" / "off" = direct
        else
            applyConfiguredProxy(client);
        creds = client.registerAccount(username, password,
                                       "progressive-ttys", regToken);
    } catch (const std::exception& e) {
        json err;
        err["error"] = std::string("registration failed: ") + e.what();
        return {400, "application/json", err.dump()};
    }
    json account = {
        {"homeserver", hs},
        {"access_token", creds.access_token},
        {"user_id", creds.user_id},
        {"device_id", creds.device_id},
    };
    if (body.contains("proxy")) account["proxy"] = proxySpec;  // inherit
    std::string id = creds.user_id.empty()
                         ? hs + "|" + creds.access_token.substr(0, 8)
                         : creds.user_id;
    TtysSession* s;
    {
        std::lock_guard<std::mutex> lock(_mu);
        s = createOrGetSession(id, account);
    }
    if (!s) {
        return {500, "application/json", R"({"error":"cannot create session"})"};
    }
    json out;
    out["session"] = s->id;
    out["user_id"] = creds.user_id;
    out["access_token"] = creds.access_token;
    out["device_id"] = creds.device_id;
    out["homeserver"] = hs;
    return {200, "application/json", out.dump()};
}

api::Response TtysApi::handleSession(const api::Request& req) {
    if (!authorized(req)) {
        return {401, "application/json", R"({"error":"unauthorized"})"};
    }
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
        return {400, "application/json", R"({"error":"bad JSON"})"};
    }
    json account = body.value("account", json::object());
    std::string hs = account.value("homeserver", "");
    std::string tok = account.value("access_token", "");
    if (hs.empty() || tok.empty()) {
        return {400, "application/json",
                R"({"error":"account.homeserver and account.access_token required"})"};
    }
    std::string id = account.value("user_id", "");
    if (id.empty()) id = hs + "|" + tok.substr(0, 8);
    std::lock_guard<std::mutex> lock(_mu);
    TtysSession* s = createOrGetSession(id, account);
    if (!s) {
        return {500, "application/json", R"({"error":"cannot create session"})"};
    }
    json out;
    out["session"] = s->id;
    out["homeserver"] = s->homeserver;
    out["sync"] = s->syncing.load() ? "syncing" : "idle";
    return {200, "application/json", out.dump()};
}

api::Response TtysApi::handleRender(const api::Request& req) {
    if (!authorized(req)) {
        return {401, "application/json", R"({"error":"unauthorized"})"};
    }
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) return {400, "application/json", R"({"error":"bad JSON"})"};
    std::string id = body.value("session", "");
    if (id.empty()) return {400, "application/json", R"({"error":"session required"})"};

    std::lock_guard<std::mutex> lock(_mu);
    TtysSession* s = findSession(id);
    if (!s) return {404, "application/json", R"({"error":"no such session"})"};

    // sync: "auto" starts the background thread, "once" does one pass now,
    // "off"/absent leaves the cache as-is. The server default (the --sync
    // flag of serve --ttys) applies when the request omits it.
    std::string sync = body.value("sync",
                                  _defaultSync.empty() ? "off" : _defaultSync);
    if (sync == "once") {
        if (s->syncing.exchange(true)) {
            // already syncing — await the in-flight pass briefly
            while (s->syncing.load())
                std::this_thread::sleep_for(100ms);
        } else {
            s->syncing = false;
            runSyncPass(*s);
        }
    } else if (sync == "auto") {
        if (!s->syncThread.joinable()) {
            s->wantsAutoSync = true;
            s->syncThread = std::thread([this, s]() {
                while (s->wantsAutoSync.load()) {
                    runSyncPass(*s);
                    std::this_thread::sleep_for(2s);
                }
            });
        }
    }

    int cols = body.value("cols", 0);
    int rows = body.value("rows", 0);
    if (cols <= 0) cols = terminalColumns();
    if (rows <= 0) rows = 24 + 5;

    std::string frame = renderFrame(*s, cols, rows);
    json out;
    out["frame"] = frame;
    out["width"] = cols;
    out["height"] = rows;
    out["sync"] = s->syncing.load() ? "syncing" : "idle";
    return {200, "application/json", out.dump()};
}

api::Response TtysApi::handleInput(const api::Request& req) {
    if (!authorized(req)) {
        return {401, "application/json", R"({"error":"unauthorized"})"};
    }
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) return {400, "application/json", R"({"error":"bad JSON"})"};
    std::string id = body.value("session", "");
    std::string input = body.value("input", "");
    if (id.empty()) return {400, "application/json", R"({"error":"session required"})"};

    std::lock_guard<std::mutex> lock(_mu);
    TtysSession* s = findSession(id);
    if (!s) return {404, "application/json", R"({"error":"no such session"})"};

    // Parse the input line like the REPL does, then run the same commands.
    cli::Args a;
    if (!input.empty()) {
        std::istringstream iss(input);
        std::vector<std::string> words;
        std::string w;
        while (iss >> w) words.push_back(w);
        a.command = words[0];
        static const std::unordered_set<std::string> noValueFlags = {
            "static", "once", "print", "json", "confirm", "debug", "ts",
            "ids", "expand", "verbose", "all", "interactive", "help",
            "cli", "ui", "ascii", "populate", "no-replies", "no-filter",
        };
        for (size_t i = 1; i < words.size(); ++i) {
            if (words[i].size() >= 2 && words[i][0] == '-' && words[i][1] == '-') {
                std::string key = words[i].substr(2);
                auto eq = key.find('=');
                if (eq != std::string::npos) {
                    a.options[key.substr(0, eq)] = key.substr(eq + 1);
                } else if (i + 1 < words.size() && !noValueFlags.count(key) &&
                           !(words[i + 1].size() >= 2 && words[i + 1][0] == '-')) {
                    a.options[key] = words[++i];
                } else {
                    a.options[key] = "true";
                }
            } else {
                a.positional.push_back(words[i]);
            }
        }
    }

    int rc = asciiReplDispatchA(s->st, *s->dbi, a);
    if (rc == 1) {
        int rb = asciiReplDispatchB(s->st, *s->dbi, a);
        if (rb == 1) {
            int rc2 = asciiAgentReplDispatch(s->st, *s->dbi, a);
            if (rc2 == 1) {
                int rg = asciiReplDispatchG(s->st, *s->dbi, a);
                if (rg == 1) {
                    int re = asciiReplDispatchE(s->st, *s->dbi, a);
                    if (re == 1) s->st.statusNote = "Unknown command '" + a.command + "'";
                }
            }
        }
    }

    int cols = body.value("cols", 0);
    int rows = body.value("rows", 0);
    if (cols <= 0) cols = terminalColumns();
    if (rows <= 0) rows = 24 + 5;
    std::string frame = renderFrame(*s, cols, rows);
    json out;
    out["frame"] = frame;
    out["width"] = cols;
    out["height"] = rows;
    return {200, "application/json", out.dump()};
}

api::Response TtysApi::handleSync(const api::Request& req) {
    if (!authorized(req)) {
        return {401, "application/json", R"({"error":"unauthorized"})"};
    }
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) return {400, "application/json", R"({"error":"bad JSON"})"};
    std::string id = body.value("session", "");
    std::lock_guard<std::mutex> lock(_mu);
    TtysSession* s = findSession(id);
    if (!s) return {404, "application/json", R"({"error":"no such session"})"};
    runSyncPass(*s);
    json out;
    out["synced"] = !s->syncing.load();
    if (!s->syncError.empty()) out["error"] = s->syncError;
    out["lastSyncMs"] = s->lastSyncMs;
    return {200, "application/json", out.dump()};
}

// GET/POST /api/ttys/proxy — manage the server-wide proxy (the full client's
// `proxy on/off`) so a thin -terminal client can drive it over HTTP.
//   GET                        -> {enabled, preset, host, port, type}
//   POST {action:"on",preset}  -> enable a named preset (from config.json)
//   POST {action:"off"}        -> disable (direct connections)
api::Response TtysApi::handleProxy(const api::Request& req) {
    if (!authorized(req)) {
        return {401, "application/json", R"({"error":"unauthorized"})"};
    }
    if (req.method == "GET") {
        progressive::desktop::ProxyConfig cfg;
        bool on = activeProxyConfig(&cfg);
        json out;
        out["enabled"] = on;
        out["preset"] = Config::instance().get("proxy_active");
        if (on) {
            out["host"] = cfg.host;
            out["port"] = cfg.port;
            out["type"] = (cfg.type == progressive::desktop::ProxyConfig::Type::Http)
                              ? "http"
                              : (cfg.type == progressive::desktop::ProxyConfig::Type::Socks5)
                                    ? "socks5"
                                    : "socks5h";
        }
        return {200, "application/json", out.dump()};
    }
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded())
        return {400, "application/json", R"({"error":"bad JSON"})"};
    std::string action = body.value("action", "");
    if (action == "off") {
        Config::instance().set("proxy_active", "");
        Config::instance().save();
        applyProxyFromConfig();
        return {200, "application/json", R"({"action":"off","enabled":false})"};
    }
    if (action == "on") {
        std::string preset = body.value("preset", "");
        if (preset.empty())
            return {400, "application/json", R"({"error":"preset required"})"};
        auto arr = Config::instance().getRaw("proxy_presets");
        bool found = false;
        if (arr.is_array())
            for (auto& e : arr)
                if (e.value("name", "") == preset) { found = true; break; }
        if (!found)
            return {400, "application/json",
                    R"({"error":"unknown preset ')" + preset + R"('"})"};
        Config::instance().set("proxy_active", preset);
        Config::instance().save();
        applyProxyFromConfig();
        return {200, "application/json",
                R"({"action":"on","preset":")" + preset + R"("})"};
    }
    return {400, "application/json", R"({"error":"unknown action (use on/off)"})"};
}

}} // namespace matrixcli::server

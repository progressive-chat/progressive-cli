#pragma once

// Serve --ttys: remote ASCII-UI rendering.
//
// The server process runs as the account owner; a THIN client (any user on
// the network) sends a JSON request with the terminal size + the account
// (homeserver/access-token/user-id/device-id) in the request body. The
// server keeps the account in RAM only (never saved to the server's home
// dir unless persist is requested) and responds with the same ASCII frame
// the local CLI would draw. Optional background auto-sync vs one-shot sync
// on request (per session, request-settable).

#include "../../lib/api/server.hpp"
#include "../../lib/api/router.hpp"
#include "../../lib/matrix/client.hpp"
#include "../ascii_state.hpp"
#include "../ascii_ui_impl.hpp"
#include <mutex>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <cstdint>
#include <functional>

namespace matrixcli { namespace server {

// One remote ttys session: RAM-held account + in-memory cache + UiState.
struct TtysSession {
    std::string id;               // session id (returned to the client)
    std::string homeserver;
    std::string accessToken;
    std::string userId;
    std::string deviceId;

    std::unique_ptr<db::Database> dbi;    // ":memory:" cache
    std::unique_ptr<matrix::Client> client;
    // Per-session proxy ("proxy" in the session/register request), kept
    // here so re-joining an existing session does not lose it.
    std::string proxySpec;
    UiState st;

    std::atomic<bool> syncing{false};     // a sync pass is running
    std::atomic<bool> wantsAutoSync{false}; // background sync thread flag
    std::thread syncThread;
    int64_t lastSyncMs = 0;
    std::string syncError;
    bool accountValidated = false;        // whoami ok
};

// The ttys API surface: one registry, thread-safe, routes added by the
// caller (cmdServe --ttys) onto the api::Server.
class TtysApi {
public:
    TtysApi() = default;
    ~TtysApi();

    void setToken(const std::string& token) { _token = token; }
    // Default sync mode for NEW sessions; per-request "sync" overrides.
    void setDefaultSync(const std::string& mode) { _defaultSync = mode; }
    // persistCachePath: non-empty = the session cache DB lives on disk (the
    // account still stays in RAM); empty = ":memory:" (default).
    void setPersistCachePath(const std::string& path) { _persistCachePath = path; }

    TtysApi(const TtysApi&) = delete;
    TtysApi& operator=(const TtysApi&) = delete;

    // Routes the API routes onto an api::Server. Call before start().
    void registerRoutes(api::Router& router);

    // Request bodies (JSON):
    //   POST /api/ttys/register {homeserver,username,password,reg_token?,proxy?} -> {session,user_id,access_token,device_id}
    //   POST /api/ttys/session {account:{...,proxy?}} -> {session, key}
    //   POST /api/ttys/render   {session, term:{cols,rows}, sync:"auto"|"once"|"off", view?...} -> {frame,width,height}
    //   POST /api/ttys/input    {session, input:"..."} -> {frame,...}
    //   POST /api/ttys/sync     {session} -> {synced,error}
    // The optional per-session "proxy" ("socks5://[u:p@]h:p" | "http://h:p"
    // | "off") overrides the server-wide `proxy on` default for THAT
    // session's homeserver traffic only.
    int activeSessionCount();

private:
    api::Response handleRegister(const api::Request& req);
    api::Response handleSession(const api::Request& req);
    api::Response handleRender(const api::Request& req);
    api::Response handleInput(const api::Request& req);
    api::Response handleSync(const api::Request& req);

    TtysSession* findSession(const std::string& id);
    TtysSession* createOrGetSession(const std::string& id,
                                    const nlohmann::json& account);
    void ensureStateLoaded(TtysSession& s);
    std::string renderFrame(TtysSession& s, int cols, int rows);
    void runSyncPass(TtysSession& s);
    bool authorized(const api::Request& req);

    std::mutex _mu;
    std::map<std::string, std::unique_ptr<TtysSession>> _sessions;
    std::string _token;          // --token X: require "Authorization: Bearer X"
    std::string _defaultSync;    // "auto" | "once" | "off"
    std::string _persistCachePath; // empty = ":memory:"
};

}} // namespace matrixcli::server

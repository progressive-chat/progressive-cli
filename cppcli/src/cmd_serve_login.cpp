#include <iostream>
#include <csignal>
#include <signal.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <set>
#include <map>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "config.hpp"
#include "media_send.hpp"
#include "main_commands.hpp"
#include "commands.hpp"
#include "core/http_client.hpp"
#include "core/crypto/media_crypto.hpp"
#include <simdjson.h>
#include "globals.hpp"
#include "pcore.hpp"
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include "core/crash_handler.hpp"
#include "server/server.hpp"
#include "server/ttys.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/tdlib/tdlib_bridge.hpp"
#include "../lib/irc/irc_client.hpp"
#include "../lib/lemmy/lemmy_client.hpp"
#include "../lib/deltachat/dc_bridge.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"
#include "../lib/util/string_utils.hpp"
#include "../lib/util/client_utils.hpp"
#ifdef __unix__
#include <sys/resource.h>
#include <sys/prctl.h>
#endif
#ifdef BUILD_TUI
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/agent_setup.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"
#endif


int cmdServe(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    // The headless ttys relay lives on the port the whole thin-client
    // ecosystem expects (scan range, docs); the web UI keeps 8080.
    int port = args.options.contains("ttys") ? 29325 : 8080;
    auto port_it = args.options.find("port");
    if (port_it == args.options.end()) port_it = args.options.find("p");
    if (port_it != args.options.end()) {
        port = std::stoi(port_it->second);
    }

    // serve --ttys: remote ASCII-UI mode. The server holds RAM-only sessions
    // (account arrives in every request; nothing is persisted on the serve
    // side) and replies with the same ASCII frames the local CLI draws.
    // --bind ADDR restricts the listeners (default 127.0.0.1); --token X
    // requires X in the Authorization header on every request.
    if (args.options.contains("ttys")) {
        std::string bindAddr = "127.0.0.1";
        if (args.options.count("bind")) bindAddr = args.options.at("bind");
        server::TtysApi ttys;
        ttys.setToken(args.options.count("token") ? args.options.at("token") : "");
        // --sync auto|once|off: the default sync behaviour for sessions (the
        // client can override per request). Default: manual (off).
        if (args.options.count("sync"))
            ttys.setDefaultSync(args.options.at("sync"));
        // --cache FILE: persist the session cache on disk; default ":memory:".
        if (args.options.count("cache"))
            ttys.setPersistCachePath(args.options.at("cache"));
        // RAM-only guarantee: proxy_active stays in memory unless explicitly
        // opted in, and core dumps are disabled so RAM secrets can't land
        // in a crash dump.
        ttys.setPersistProxy(args.options.count("persist-proxy") > 0);
#ifdef __unix__
        struct rlimit cl {};
        cl.rlim_cur = 0; cl.rlim_max = 0;
        if (setrlimit(RLIMIT_CORE, &cl) != 0)
            util::Logger::instance().warn("RAM-only: setrlimit(CORE) failed");
#ifdef __linux__
        if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
            util::Logger::instance().warn("RAM-only: PR_SET_DUMPABLE failed");
#endif
#endif
        api::Server api_server(port, bindAddr);
        api::Router router;
        ttys.registerRoutes(router);
        router.apply(api_server);
        try {
            api_server.start();
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            std::cerr << "  Is the port already in use? Try a different one:"
                      << " progressive-cli serve --ttys --port <other>" << std::endl;
            return 1;
        }
        std::cout << "ttys server running on http://" << bindAddr << ":" << port
                  << " (sessions: " << ttys.activeSessionCount() << ")" << std::endl;
        std::cout << "RAM-only: cache=:memory:"
                  << (args.options.count("cache") ? "(overridden)" : "")
                  << ", config-writes="
                  << (args.options.count("persist-proxy") ? "on" : "off")
                  << ", core-dumps=off" << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        api_server.stop();
        return 0;
    }

    Config::instance().load("config.json");

    bool demo_mode = args.options.contains("demo");
    if (!demo_mode && args.command == "demo") demo_mode = true;

    server::ServerMode mode;
    std::string homeserver = "https://matrix.org";

    if (demo_mode) {
        mode = server::ServerMode::Demo;
    } else if (pcore::init() && pcore::loadSavedSession()) {
        mode = server::ServerMode::Matrix;
        homeserver = pcore::core().client->account().homeserverUrl;
        util::Logger::instance().info("Loaded saved session for " + pcore::core().client->account().userId);

        // E2EE bootstrap (olm account + device keys) — non-fatal on failure.
        std::string e2ee_note = pcore::bootstrap();
        if (!e2ee_note.empty()) util::Logger::instance().warn(e2ee_note);

        // Feed the offline cache from every /sync response (the bridge keeps
        // view/rooms/search/API working on the legacy db::Database store).
        pcore::startSync([](const progressive::desktop::FastSyncResponse& resp) {
            pcore::feedCache(resp);
        });
        util::Logger::instance().info("Background sync started (vendored desktop core)");
    } else {
        mode = server::ServerMode::WebProxy;
        util::Logger::instance().info("No saved session — falling back to web proxy mode");
    }

    server::APIServer api_server(port, mode, homeserver, pcore::core().client);
    try {
        api_server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << "  Is the port already in use? Try a different one:"
                  << " progressive-cli serve --port <other>" << std::endl;
        pcore::stopSync();
        return 1;
    }

    std::cout << "API server running on http://localhost:" << port << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    pcore::stopSync();
    api_server.stop();
    return 0;
}int cmdLogin(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    Config::instance().load("config.json");

    std::string homeserver;
    auto hs_it = args.options.find("homeserver");
    if (hs_it != args.options.end()) {
        homeserver = hs_it->second;
    } else if (!Config::instance().homeserverURL().empty()) {
        homeserver = Config::instance().homeserverURL();
    } else {
        std::cerr << "Error: --homeserver required" << std::endl;
        return 1;
    }

    bool json_out = args.options.count("json");

    // --access-token: install an EXISTING session (an access token from the
    // desktop app / Element). whoami resolves the user+device; the session
    // is persisted + E2EE bootstrapped like a login.
    auto at_it = args.options.find("access-token");
    if (at_it != args.options.end()) {
        if (homeserver.empty()) {
            std::cerr << "Error: --homeserver required" << std::endl;
            return 1;
        }
        if (!pcore::init()) return 1;
        auto& core = pcore::core();
        std::string resolvedHs = homeserver;
        while (!resolvedHs.empty() && resolvedHs.back() == '/') resolvedHs.pop_back();
        progressive::desktop::AccountInfo staged;
        staged.homeserverUrl = resolvedHs;
        staged.accessToken = at_it->second;
        core.client->setAccount(staged);
        auto who = progressive::desktop::httpGet(
            resolvedHs + "/_matrix/client/v3/account/whoami",
            {{"Authorization", "Bearer " + at_it->second}}, 15000);
        if (!who.success || who.statusCode != 200) {
            std::cerr << "Login failed: access token rejected (HTTP " << who.statusCode
                      << "): " << who.errorMessage << std::endl;
            return 1;
        }
        std::string whoUser, whoDevice;
        {
            simdjson::dom::parser p;
            auto doc = p.parse(who.body);
            if (doc.error() == simdjson::SUCCESS) {
                auto u = doc.value()["user_id"].get_string();
                if (u.error() == simdjson::SUCCESS) whoUser = std::string(u.value());
                auto d = doc.value()["device_id"].get_string();
                if (d.error() == simdjson::SUCCESS) whoDevice = std::string(d.value());
            }
        }
        if (whoUser.empty()) {
            std::cerr << "Login failed: whoami returned no user_id" << std::endl;
            return 1;
        }
        auto acct = core.client->account();
        acct.userId = whoUser;
        acct.deviceId = whoDevice;
        core.client->setAccount(acct);
        core.client->persistSession();

        Config::instance().set("homeserver_url", acct.homeserverUrl);
        Config::instance().set("access_token", acct.accessToken);
        Config::instance().set("user_id", acct.userId);
        Config::instance().set("device_id", acct.deviceId);
        Config::instance().save();

        db::Database dbi;
        dbi.open("matrixcli.db");
        db::StoredAccount acc;
        acc.homeserver_url = acct.homeserverUrl;
        acc.user_id = acct.userId;
        acc.access_token = acct.accessToken;
        acc.device_id = acct.deviceId;
        dbi.saveAccount(acc);

        std::string e2ee_note = pcore::bootstrap();
        if (json_out) {
            nlohmann::json j;
            j["user_id"] = acct.userId;
            j["device_id"] = acct.deviceId;
            j["homeserver"] = acct.homeserverUrl;
            j["e2ee"] = e2ee_note.empty();
            std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Logged in as " << acct.userId << " (device " << acct.deviceId << ")" << std::endl;
            if (!e2ee_note.empty()) std::cout << "Warning: " << e2ee_note << std::endl;
            else std::cout << "E2EE ready — device keys uploaded." << std::endl;
        }
        return 0;
    }

    // SSO / OIDC-style login (MSC3861 family). The homeserver redirects the
    // browser back with ?loginToken=…; the user pastes that token (or the full
    // redirect URL) and we exchange it for a session via m.login.token.
    if (args.options.count("sso")) {
        if (homeserver.empty()) {
            std::cerr << "Error: --homeserver required" << std::endl;
            return 1;
        }
        if (!pcore::init()) return 1;
        auto& core = pcore::core();
        std::string resolvedHs;
        if (homeserver.rfind("http://", 0) == 0 || homeserver.rfind("https://", 0) == 0) {
            resolvedHs = homeserver;
            while (!resolvedHs.empty() && resolvedHs.back() == '/') resolvedHs.pop_back();
        } else {
            auto disc = core.client->discoverHomeserver(homeserver);
            if (!disc.ok || disc.data.empty()) {
                std::cerr << "Login failed: cannot resolve homeserver " << homeserver << std::endl;
                return 1;
            }
            resolvedHs = disc.data;
        }
        matrix::Client ssoClient;
        ssoClient.setHomeserverURL(resolvedHs);
        std::string ssoUrl = ssoClient.getSSOLoginURL(resolvedHs + "/_matrix/client/");
        if (ssoUrl.empty()) {
            std::cerr << "This homeserver does not advertise SSO login." << std::endl;
            return 1;
        }
        std::cout << "Open this URL in your browser and authenticate:\n\n  " << ssoUrl
                  << "\n\nThen paste the ?loginToken= value (or the whole redirect URL): " << std::flush;
        std::string pasted;
        std::getline(std::cin, pasted);
        std::string token;
        auto pos = pasted.find("loginToken=");
        if (pos != std::string::npos) token = pasted.substr(pos + 11);
        else token = pasted;
        // Trim trailing whitespace / newline artifacts.
        while (!token.empty() && (token.back() == ' ' || token.back() == '\r' || token.back() == '\n'))
            token.pop_back();
        if (token.empty()) { std::cerr << "No login token supplied.\n"; return 1; }
        auto r = ssoClient.loginSSO(token, "matrixcli");
        if (!r.valid()) {
            std::cerr << "SSO login failed." << std::endl;
            return 1;
        }
        progressive::desktop::AccountInfo staged;
        staged.homeserverUrl = resolvedHs;
        staged.accessToken = r.access_token;
        staged.userId = r.user_id;
        staged.deviceId = r.device_id;
        core.client->setAccount(staged);
        core.client->persistSession();
        auto acct = core.client->account();
        Config::instance().set("homeserver_url", acct.homeserverUrl);
        Config::instance().set("access_token", acct.accessToken);
        Config::instance().set("user_id", acct.userId);
        Config::instance().set("device_id", acct.deviceId);
        Config::instance().save();
        std::string e2ee_note = pcore::bootstrap();
        if (json_out) {
            nlohmann::json j; j["user_id"] = acct.userId; j["device_id"] = acct.deviceId;
            j["homeserver"] = acct.homeserverUrl; j["e2ee"] = e2ee_note.empty();
            std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Logged in as " << acct.userId << " (device " << acct.deviceId << ")" << std::endl;
            if (!e2ee_note.empty()) std::cout << "Warning: " << e2ee_note << std::endl;
            else std::cout << "E2EE ready — device keys uploaded." << std::endl;
        }
        return 0;
    }

    // Password login via the vendored desktop core (lib/ecore).
    auto token_it = args.options.find("token");
    auto reg_it = args.options.find("register");
    if (token_it == args.options.end() && reg_it == args.options.end()) {
        std::string username;
        // --username accepts BOTH the localpart ("me") and a full MXID
        // ("@me:server"); --mxid is an alias that expects a full Matrix ID.
        auto user_it = args.options.find("username");
        auto mxid_it = args.options.find("mxid");
        if (user_it != args.options.end()) {
            username = user_it->second;
        } else if (mxid_it != args.options.end()) {
            username = mxid_it->second;
        } else if (args.positional.size() >= 1) {
            username = args.positional[0];
        }

        std::string password;
        auto pass_it = args.options.find("password");
        if (pass_it != args.options.end()) {
            password = pass_it->second;
        } else if (args.positional.size() >= 2) {
            password = args.positional[1];
        }

        // --interactive: prompt for whatever is missing (password hidden).
        if (args.options.count("interactive")) {
            if (username.empty()) {
                std::cout << "Username (localpart, e.g. me): " << std::flush;
                std::getline(std::cin, username);
            }
            if (password.empty()) {
                std::cout << "Password: " << std::flush;
                struct termios oldt, newt;
                tcgetattr(STDIN_FILENO, &oldt);
                newt = oldt; newt.c_lflag &= ~ECHO;
                tcsetattr(STDIN_FILENO, TCSANOW, &newt);
                std::getline(std::cin, password);
                tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                std::cout << std::endl;
            }
        }

        // Login takes the LOCALPART: strip @ and :server if the user passed
        // a full Matrix ID (via --username, --mxid, or the prompt) — some
        // homeservers (matrix.org) reject m.id.user with a full MXID.
        if (!username.empty() && username[0] == '@') {
            auto colon = username.find(':');
            if (colon != std::string::npos) username = username.substr(1, colon - 1);
            else username = username.substr(1);
        }

        // Report ALL missing credentials at once (both when neither given),
        // and point at --interactive when the prompt flag was not used.
        if (username.empty() && password.empty()) {
            std::cerr << "Error: --username and --password required"
                      << " (e.g. --username @me:server --password s3cret)"
                      << " — or add --interactive to enter them interactively"
                      << std::endl;
            return 1;
        }
        if (username.empty()) {
            std::cerr << "Error: --username required"
                      << " — or add --interactive to enter it interactively"
                      << std::endl;
            return 1;
        }
        if (password.empty()) {
            std::cerr << "Error: --password required"
                      << " — or add --interactive to enter it interactively"
                      << std::endl;
            return 1;
        }

        if (!pcore::init()) return 1;
        auto& core = pcore::core();

        // Resolve the homeserver. Explicit scheme (http:// for local dev,
        // https:// in production) is honored as-is — the core's
        // formatServerUrl forces https, which breaks local plain-HTTP
        // homeservers. Scheme-less input goes through well-known discovery.
        std::string resolvedHs;
        if (homeserver.rfind("http://", 0) == 0 || homeserver.rfind("https://", 0) == 0) {
            resolvedHs = homeserver;
            while (!resolvedHs.empty() && resolvedHs.back() == '/') resolvedHs.pop_back();
        } else {
            auto disc = core.client->discoverHomeserver(homeserver);
            if (!disc.ok || disc.data.empty()) {
                std::cerr << "Login failed: cannot resolve homeserver " << homeserver << std::endl;
                return 1;
            }
            resolvedHs = disc.data;
        }
        progressive::desktop::AccountInfo staged;
        staged.homeserverUrl = resolvedHs;
        core.client->setAccount(staged);

        auto r = core.client->loginWithPassword(username, password, "matrixcli");
        if (!r.ok) {
            std::string err = r.error.message.empty() ? "login failed" : r.error.message;
            if (!r.error.code.empty()) err = "[" + r.error.code + "] " + err;
            std::cerr << "Login failed: " << err << " (HTTP " << r.httpStatus << ")" << std::endl;
            return 1;
        }

        auto acct = core.client->account();
        core.client->persistSession();

        // Compatibility: also record the session in config.json.
        Config::instance().set("homeserver_url", acct.homeserverUrl);
        Config::instance().set("access_token", acct.accessToken);
        Config::instance().set("user_id", acct.userId);
        Config::instance().set("device_id", acct.deviceId);
        Config::instance().save();

        std::string e2ee_note = pcore::bootstrap();

        if (json_out) {
            nlohmann::json j;
            j["user_id"] = acct.userId;
            j["device_id"] = acct.deviceId;
            j["homeserver"] = acct.homeserverUrl;
            j["e2ee"] = e2ee_note.empty();
            std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Logged in as " << acct.userId << " (device " << acct.deviceId << ")" << std::endl;
            if (!e2ee_note.empty()) std::cout << "Warning: " << e2ee_note << std::endl;
            else std::cout << "E2EE ready — device keys uploaded." << std::endl;
        }
        return 0;
    }

    // Registration via the vendored desktop core (lib/ecore) — supports
    // m.login.registration_token via --reg-token.
    if (reg_it != args.options.end()) {
        std::string username;
        auto user_it = args.options.find("username");
        if (user_it != args.options.end()) {
            username = user_it->second;
        } else if (args.positional.size() >= 1) {
            username = args.positional[0];
        }

        std::string password;
        auto pass_it = args.options.find("password");
        if (pass_it != args.options.end()) {
            password = pass_it->second;
        } else if (args.positional.size() >= 2) {
            password = args.positional[1];
        }

        std::string regToken;
        auto rt_it = args.options.find("reg-token");
        if (rt_it != args.options.end()) regToken = rt_it->second;
        if (regToken == "true") regToken = "";  // bare --reg-token

        // --interactive: prompt for whatever is missing (password hidden).
        if (args.options.count("interactive")) {
            if (username.empty()) {
                std::cout << "Username (localpart, e.g. me): " << std::flush;
                std::getline(std::cin, username);
            }
            if (password.empty()) {
                std::cout << "Password: " << std::flush;
                struct termios oldt, newt;
                tcgetattr(STDIN_FILENO, &oldt);
                newt = oldt; newt.c_lflag &= ~ECHO;
                tcsetattr(STDIN_FILENO, TCSANOW, &newt);
                std::getline(std::cin, password);
                tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                std::cout << std::endl;
            }
            if (regToken.empty() && args.options.count("reg-token")) {
                std::cout << "Registration token: " << std::flush;
                std::getline(std::cin, regToken);
            }
        }

        if (username.empty()) {
            std::cerr << "Error: --username required for registration"
                      << " — or add --interactive to enter it interactively"
                      << std::endl;
            return 1;
        }
        // Registration takes the LOCALPART: strip @ and :server if the user
        // passed a full Matrix ID (same as the desktop login dialog).
        if (!username.empty() && username[0] == '@') {
            auto colon = username.find(':');
            if (colon != std::string::npos) username = username.substr(1, colon - 1);
            else username = username.substr(1);
        }

        if (password.empty()) {
            std::cerr << "Error: --password required for registration"
                      << " — or add --interactive to enter it interactively"
                      << std::endl;
            return 1;
        }

        if (!pcore::init()) return 1;
        auto& core = pcore::core();

        // Resolve the homeserver (same rules as login: explicit scheme honored,
        // scheme-less input goes through well-known discovery).
        std::string resolvedHs;
        if (homeserver.rfind("http://", 0) == 0 || homeserver.rfind("https://", 0) == 0) {
            resolvedHs = homeserver;
            while (!resolvedHs.empty() && resolvedHs.back() == '/') resolvedHs.pop_back();
        } else {
            auto disc = core.client->discoverHomeserver(homeserver);
            if (!disc.ok || disc.data.empty()) {
                std::cerr << "Registration failed: cannot resolve homeserver " << homeserver << std::endl;
                return 1;
            }
            resolvedHs = disc.data;
        }

        auto r = core.client->registerAccount(username, password, resolvedHs, regToken);
        if (!r.ok) {
            std::string err = r.error.message.empty() ? "registration failed" : r.error.message;
            if (!r.error.code.empty()) err = "[" + r.error.code + "] " + err;
            std::cerr << "Registration failed: " << err << " (HTTP " << r.httpStatus << ")" << std::endl;
            return 1;
        }

        // registerAccount returns the account in r.data without installing it —
        // install, persist (session.db), and bootstrap E2EE like a login.
        core.client->setAccount(r.data);
        auto acct = core.client->account();
        core.client->persistSession();

        // Compatibility: also record the session in config.json.
        Config::instance().set("homeserver_url", acct.homeserverUrl);
        Config::instance().set("access_token", acct.accessToken);
        Config::instance().set("user_id", acct.userId);
        Config::instance().set("device_id", acct.deviceId);
        Config::instance().save();

        std::string e2ee_note = pcore::bootstrap();

        if (json_out) {
            nlohmann::json j;
            j["user_id"] = acct.userId;
            j["device_id"] = acct.deviceId;
            j["homeserver"] = acct.homeserverUrl;
            j["e2ee"] = e2ee_note.empty();
            std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Registered as " << acct.userId << " (device " << acct.deviceId << ")" << std::endl;
            if (!e2ee_note.empty()) std::cout << "Warning: " << e2ee_note << std::endl;
            else std::cout << "E2EE ready — device keys uploaded." << std::endl;
        }
        return 0;
    }

    // Token login: legacy path on the old client (transitional).
    matrix::Client client;
    client.setHomeserverURL(homeserver);
    // Route the legacy client through the same proxy as the core (if set).
    extern bool activeProxyConfig(progressive::desktop::ProxyConfig* out);
    progressive::desktop::ProxyConfig activeProxy;
    if (activeProxyConfig(&activeProxy)) {
        http::ProxyConfig legacyProxy;
        legacyProxy.host = activeProxy.host;
        legacyProxy.port = activeProxy.port;
        legacyProxy.type = (activeProxy.type == progressive::desktop::ProxyConfig::Type::Http)
                               ? http::ProxyType::HTTP : http::ProxyType::SOCKS5;
        legacyProxy.username = activeProxy.username;
        legacyProxy.password = activeProxy.password;
        client.setProxy(legacyProxy);
    }

    try {
        auto creds = client.loginToken(token_it->second);
        std::cout << "Logged in as " << creds.user_id << std::endl;

        Config::instance().set("homeserver_url", homeserver);
        Config::instance().set("access_token", creds.access_token);
        Config::instance().set("user_id", creds.user_id);
        Config::instance().set("device_id", creds.device_id);
        Config::instance().save();

        db::Database dbi;
        dbi.open("matrixcli.db");
        db::StoredAccount acc;
        acc.homeserver_url = homeserver;
        acc.user_id = creds.user_id;
        acc.access_token = creds.access_token;
        acc.device_id = creds.device_id;
        dbi.saveAccount(acc);
    } catch (const std::exception& e) {
        std::cerr << "Login failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
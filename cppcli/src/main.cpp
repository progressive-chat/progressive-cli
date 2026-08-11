#include <iostream>
#include <csignal>
#include <signal.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "config.hpp"
#include "cli/args.hpp"
#include "commands.hpp"
#include "core/http_client.hpp"
#include "core/crypto/media_crypto.hpp"
#include <simdjson.h>
#include "globals.hpp"
#include "pcore.hpp"
#include "ascii_ui.hpp"
#include "core/crash_handler.hpp"
#include "server/server.hpp"
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

#ifdef BUILD_TUI
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"
#endif

namespace {

std::atomic<bool> g_running{true};

} // anonymous namespace

// Global bridge instances (defined here, declared in globals.hpp)
namespace matrixcli {
    std::atomic<bool> g_interrupted{true};
    tdlib::TdBridge g_tdlib;
    lemmy::LemmyClient g_lemmy;
    deltachat::DcBridge g_dc;
    std::map<std::string, std::vector<std::pair<std::string, int>>> g_msgQueue;
    std::mutex g_queueMutex;
    util::TypingMonitor g_typing;
    std::vector<std::string> g_notifyKeywords;
}

void signalHandler(int) {
    g_running = false;
    matrixcli::g_interrupted = false;
}

int cmdServe(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    int port = 8080;
    auto port_it = args.options.find("port");
    if (port_it == args.options.end()) port_it = args.options.find("p");
    if (port_it != args.options.end()) {
        port = std::stoi(port_it->second);
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
                  << " matrixcli serve --port <other>" << std::endl;
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
}

int cmdLogin(const matrixcli::cli::Args& args) {
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
        } else {
            std::cerr << "Error: --username required for registration" << std::endl;
            return 1;
        }
        // Registration takes the LOCALPART: strip @ and :server if the user
        // passed a full Matrix ID (same as the desktop login dialog).
        if (!username.empty() && username[0] == '@') {
            auto colon = username.find(':');
            if (colon != std::string::npos) username = username.substr(1, colon - 1);
            else username = username.substr(1);
        }

        std::string password;
        auto pass_it = args.options.find("password");
        if (pass_it != args.options.end()) {
            password = pass_it->second;
        } else if (args.positional.size() >= 2) {
            password = args.positional[1];
        } else {
            std::cerr << "Error: --password required for registration" << std::endl;
            return 1;
        }

        std::string regToken;
        auto rt_it = args.options.find("reg-token");
        if (rt_it != args.options.end()) regToken = rt_it->second;

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
    if (Config::instance().get("proxy_enabled") == "true") {
        http::ProxyConfig legacyProxy;
        legacyProxy.host = Config::instance().get("proxy_host");
        legacyProxy.port = std::stoi(Config::instance().get("proxy_port").empty()
                                         ? "0" : Config::instance().get("proxy_port"));
        std::string ptype = Config::instance().get("proxy_type");
        legacyProxy.type = (ptype == "http") ? http::ProxyType::HTTP : http::ProxyType::SOCKS5;
        legacyProxy.username = Config::instance().get("proxy_user");
        legacyProxy.password = Config::instance().get("proxy_pass");
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

int cmdStatus(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    bool watch = args.options.count("watch") || args.options.count("live");
    int interval = 3;
    auto iv = args.options.find("watch"); if (iv != args.options.end() && !iv->second.empty() && iv->second != "true") interval = std::stoi(iv->second);
    iv = args.options.find("live"); if (iv != args.options.end() && !iv->second.empty() && iv->second != "true") interval = std::stoi(iv->second);

    // Room drill-down
    bool drill = !args.positional.empty();

    bool json_out = args.options.count("json");

    // ecore session (vendored desktop core) takes precedence over the legacy db.
    if (pcore::init() && pcore::loadSavedSession()) {
        auto& core = pcore::core();
        auto acct = core.client->account();
        auto syncToken = core.store->loadSyncToken(acct.userId);
        bool synced = syncToken.has_value() && !syncToken->empty();
        bool e2ee = core.sync->decryptor()->isInitialized();

        // Cache stats from the legacy offline store (fed by the sync bridge).
        int rooms_c = 0, msgs_c = 0, notif_c = 0;
        db::Database dbi;
        if (dbi.open("matrixcli.db")) {
            auto rooms = dbi.listRooms();
            rooms_c = (int)rooms.size();
            for (auto& r : rooms) msgs_c += dbi.getEventCount(r.value("room_id", ""));
            notif_c = dbi.getNotificationCount();
        }

        if (json_out) {
            nlohmann::json j;
            j["logged_in"] = true;
            j["user_id"] = acct.userId;
            j["homeserver"] = acct.homeserverUrl;
            j["device_id"] = acct.deviceId;
            j["synced"] = synced;
            j["e2ee"] = e2ee;
            j["rooms"] = rooms_c;
            j["messages"] = msgs_c;
            j["unread"] = notif_c;
            std::cout << j.dump() << std::endl;
            return 0;
        }

        std::cout << ANSI_BOLD "\n  matrixcli status\n" ANSI_RESET << std::endl;
        std::cout << ANSI_CYAN "\n  ── Account ──\n" ANSI_RESET;
        std::cout << "  User:       " << acct.userId << std::endl;
        std::cout << "  Homeserver: " << acct.homeserverUrl << std::endl;
        std::cout << "  Device:     " << acct.deviceId << std::endl;
        std::cout << ANSI_CYAN "\n  ── E2EE ──\n" ANSI_RESET;
        std::cout << "  Device keys: " << (e2ee ? ANSI_GREEN "● ready" ANSI_RESET : "○ not initialized") << std::endl;
        std::cout << ANSI_CYAN "\n  ── Sync ──\n" ANSI_RESET;
        std::cout << "  Status:     " << (synced ? ANSI_GREEN "● synced" ANSI_RESET : "○ not synced — run 'matrixcli serve'") << std::endl;
        std::cout << ANSI_CYAN "\n  ── Cache ──\n" ANSI_RESET;
        std::cout << "  Rooms:      " << rooms_c << std::endl;
        std::cout << "  Messages:   " << msgs_c << std::endl;
        std::cout << "  Unread:     " << notif_c << std::endl;
        std::cout << "\n  • Run " ANSI_BOLD "matrixcli serve" ANSI_RESET " to sync messages into the cache\n";
        return 0;
    }

    if (json_out) {
        Config::instance().load("config.json");
        db::Database dbi;
        dbi.open("matrixcli.db");
        auto acc = dbi.loadAccount();

        nlohmann::json j;
        j["logged_in"] = acc.is_logged_in();
        if (acc.is_logged_in()) {
            j["user_id"] = acc.user_id;
            j["homeserver"] = acc.homeserver_url;
            j["device_id"] = acc.device_id;
            j["synced"] = !acc.next_batch.empty();

            auto rooms = dbi.listRooms();
            int total_msgs = 0;
            for (auto& r : rooms) total_msgs += dbi.getEventCount(r.value("room_id", ""));
            j["rooms"] = rooms.size();
            j["messages"] = total_msgs;
            j["unread"] = dbi.getNotificationCount();

            j["protocols"] = {
                {"matrix", "active"},
                {"irc", "not connected"},
                {"tdlib", g_tdlib.isAvailable() ? (g_tdlib.authState() == tdlib::TdAuthState::Ready ? "ready" : "configuring") : "not installed"},
                {"lemmy", g_lemmy.isLoggedIn() ? "logged in" : "not logged in"},
                {"deltachat", g_dc.isAvailable() ? (g_dc.isConfigured() ? "configured" : "not configured") : "not installed"}
            };
            struct stat st;
            j["storage_kb"] = stat("matrixcli.db", &st) == 0 ? st.st_size / 1024 : 0;
        }
        std::cout << j.dump() << std::endl;
        return 0;
    }

    auto printStatus = [&]() {
    if (watch) std::cout << "\033[2J\033[H"; // clear screen

    Config::instance().load("config.json");

    db::Database dbi;
    dbi.open("matrixcli.db");
    auto acc = dbi.loadAccount();

    std::cout << ANSI_BOLD "\n  matrixcli status\n" ANSI_RESET << std::endl;

    if (acc.is_logged_in()) {
        // Account
        std::cout << ANSI_CYAN "\n  ── Account ──\n" ANSI_RESET;
        std::cout << "  User:       " << acc.user_id << std::endl;
        std::cout << "  Homeserver: " << acc.homeserver_url << std::endl;
        std::cout << "  Device:     " << acc.device_id << std::endl;

        // Sync
        bool synced = !acc.next_batch.empty();
        std::cout << ANSI_CYAN "\n  ── Sync ──\n" ANSI_RESET;
        std::cout << "  Status:     " << (synced ? ANSI_GREEN "● online" ANSI_RESET : ANSI_RED "○ offline" ANSI_RESET) << std::endl;
        if (synced) {
            std::cout << "  Token:      " << acc.next_batch.substr(0, 20) << "..." << std::endl;
            // Show last sync time if we can determine it
            try {
                struct stat st2;
                if (stat("matrixcli.db", &st2) == 0)
                    std::cout << "  Last sync:  " << relativeTime(st2.st_mtime * 1000) << std::endl;
            } catch (...) {}
        }

        // E2EE
        std::cout << ANSI_CYAN "\n  ── Encryption ──\n" ANSI_RESET;
        matrix::Client tmpClient;
        tmpClient.setHomeserverURL(acc.homeserver_url);
        tmpClient.setAccessToken(acc.access_token);
        try {
            if (tmpClient.initCrypto(acc.user_id, acc.device_id))
                std::cout << "  Device keys: " ANSI_GREEN "● ready" ANSI_RESET << std::endl;
            else std::cout << "  Device keys: ○ not initialized" << std::endl;
        } catch (...) { std::cout << "  Device keys: ○ error" << std::endl; }
        try {
            auto devices = tmpClient.getDevices();
            int dev_count = devices.value("devices", nlohmann::json::array()).size();
            std::cout << "  Devices:     " << dev_count << std::endl;
        } catch (...) {}

        // Server info
        std::cout << ANSI_CYAN "\n  ── Server ──\n" ANSI_RESET;
        try {
            auto versions = tmpClient.getServerVersions();
            std::cout << "  Versions:    " << (versions.versions.empty() ? "?" : versions.versions[0].substr(0, 6)) << std::endl;
        } catch (...) { std::cout << "  Versions:    unreachable" << std::endl; }

        // Rooms
        auto rooms = dbi.listRooms();
        int total_msgs = 0;
        for (auto& r : rooms) total_msgs += dbi.getEventCount(r.value("room_id", ""));
        int total_notif = dbi.getNotificationCount();
        int highlight = 0;
        auto notifs = dbi.getNotifications(99, true);
        for (auto& n : notifs) if (n.value("highlight", false)) highlight++;

        std::cout << ANSI_CYAN "\n  ── Rooms ──\n" ANSI_RESET;
        std::cout << "  Rooms:      " << rooms.size() << std::endl;
        std::cout << "  Messages:   " << total_msgs << std::endl;
        std::cout << "  Unread:     " << total_notif;
        if (highlight > 0) std::cout << ANSI_BOLD " (" << highlight << " highlights)" ANSI_RESET;
        std::cout << std::endl;

        // Room list (top 5 by notification)
        if (total_notif > 0) {
            std::vector<std::pair<int, std::string>> sorted;
            for (auto& r : rooms) {
                int n = dbi.getNotificationCount(r.value("room_id", ""));
                if (n > 0) sorted.push_back({n, r.value("name", r.value("room_id", ""))});
            }
            std::sort(sorted.rbegin(), sorted.rend());
            int show = std::min(5, (int)sorted.size());
            for (int i = 0; i < show; i++)
                std::cout << "    " << sorted[i].second << " [" << sorted[i].first << "]" << std::endl;

            // Last 3 notification previews
            auto recent = dbi.getNotifications(3, true);
            if (!recent.empty()) {
                std::cout << "\n  Last notifications:" << std::endl;
                for (auto& n : recent) {
                    std::string sender = n.value("sender", "?");
                    auto at = sender.find(':'); if (at != std::string::npos) sender = sender.substr(1, at - 1);
                    std::string body = n.value("body", "");
                    if (body.size() > 50) body = body.substr(0, 47) + "...";
                    bool hl = n.value("highlight", false);
                    std::cout << "    " << (hl ? ANSI_BOLD "★" ANSI_RESET : " ") << " " << sender << ": " << body << std::endl;
                }
            }
        }

        // Protocols
        std::cout << ANSI_CYAN "\n  ── Protocols ──\n" ANSI_RESET;
        std::cout << "  Matrix:     " ANSI_GREEN "● active" ANSI_RESET << std::endl;
        std::cout << "  IRC:        " << "○ not connected" << std::endl;
        std::cout << "  TDLib:      " << (g_tdlib.isAvailable() ? (g_tdlib.authState() == tdlib::TdAuthState::Ready ? ANSI_GREEN "● ready" ANSI_RESET : "○ configuring") : "○ not installed") << std::endl;
        std::cout << "  Lemmy:      " << (g_lemmy.isLoggedIn() ? ANSI_GREEN "● logged in" ANSI_RESET : "○ not logged in") << std::endl;
        std::cout << "  DeltaChat:  " << (g_dc.isAvailable() ? (g_dc.isConfigured() ? ANSI_GREEN "● configured" ANSI_RESET : "○ not configured") : "○ not installed") << std::endl;

        // Storage
        std::cout << ANSI_CYAN "\n  ── Storage ──\n" ANSI_RESET;
        struct stat st;
        int db_size = stat("matrixcli.db", &st) == 0 ? st.st_size / 1024 : 0;
        // Room drill-down
        if (drill) {
            std::string query = args.positional[0];
            auto rooms = dbi.listRooms();
            std::string room_id;
            for (auto& r : rooms) {
                std::string name = r.value("name", "");
                if (name == query || name.find(query) == 0 || r.value("room_id", "") == query) {
                    room_id = r.value("room_id", "");
                    break;
                }
            }
            if (!room_id.empty()) {
                std::cout << ANSI_CYAN "\n  ── Room Detail ──\n" ANSI_RESET;
                for (auto& r : rooms) {
                    if (r.value("room_id", "") == room_id) {
                        std::cout << "  Topic:      " << r.value("topic", "(none)") << std::endl;
                        std::cout << "  Members:    " << r.value("member_count", 0) << std::endl;
                        std::cout << "  E2EE:       " << (r.value("is_encrypted", false) ? ANSI_GREEN "yes" ANSI_RESET : "no") << std::endl;
                        std::cout << "  DM:         " << (r.value("is_direct", false) ? "yes" : "no") << std::endl;
                    }
                }
                // Last 3 messages
                auto msgs = dbi.getEvents(room_id, 3);
                if (!msgs.empty()) {
                    std::cout << "\n  Last messages:" << std::endl;
                    for (auto& ev : msgs) {
                        std::string sender = ev.sender;
                        auto at = sender.find(':'); if (at != std::string::npos) sender = sender.substr(1, at - 1);
                        std::string body = ev.content.value("body", "(no body)");
                        if (body.size() > 60) body = body.substr(0, 57) + "...";
                        std::cout << "    " << sender << ": " << body << std::endl;
                    }
                }
                // Color-coded activity (messages in last 24h)
                int recent = 0;
                int64_t day_ago = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() - 86400;
                for (auto& ev : dbi.getEvents(room_id, 100)) {
                    if (ev.origin_server_ts / 1000 > day_ago) recent++;
                }
                std::string heat = recent > 20 ? ANSI_RED "● hot" : recent > 5 ? ANSI_YELLOW "● warm" : ANSI_GREEN "● quiet";
                std::cout << "\n  Activity:    " << heat << ANSI_RESET " (" << recent << " in 24h)" << std::endl;
            }
        }
    } else if (!Config::instance().accessToken().empty()) {
        std::cout << "Logged in as " << Config::instance().userId() << std::endl;
        std::cout << "Homeserver: " << Config::instance().homeserverURL() << std::endl;
        std::cout << "Device ID: " << Config::instance().deviceId() << std::endl;
    } else {
        std::cout << "\n  Not logged in. Use 'matrixcli login' to authenticate.\n";
        std::cout << "  Or try offline demo: matrixcli demo populate\n";
    }

    // Smart suggestions
    {
        std::cout << ANSI_CYAN "\n  ── Suggestions ──\n" ANSI_RESET;
        bool synced = !acc.next_batch.empty();
        int notif = dbi.getNotificationCount();
        if (!synced && acc.is_logged_in())
            std::cout << "  • Run " ANSI_BOLD "matrixcli serve" ANSI_RESET " to start syncing\n";
        if (notif > 0)
            std::cout << "  • " << notif << " unread — " ANSI_BOLD "matrixcli view room" ANSI_RESET " to read\n";
        if (g_tdlib.isAvailable() && g_tdlib.authState() != tdlib::TdAuthState::Ready)
            std::cout << "  • TDLib ready — " ANSI_BOLD "matrixcli td login" ANSI_RESET " for Telegram\n";
        if (!g_lemmy.isLoggedIn())
            std::cout << "  • Lemmy available — " ANSI_BOLD "matrixcli lemmy login" ANSI_RESET "\n";
        std::cout << "  • All commands: " ANSI_BOLD "matrixcli --help" ANSI_RESET "\n";
    }
    }; // end printStatus lambda

    printStatus();

    if (watch) {
        std::cout << "\n" ANSI_DIM "  Watching (Ctrl+C to stop, " << interval << "s interval)" ANSI_RESET << std::endl;
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            printStatus();
        }
    }

    return 0;
}

int cmdRooms(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cerr << "Cannot open database" << std::endl;
        return 1;
    }
    auto rooms = dbi.listRooms();

    bool json_out = args.options.count("json");
    if (json_out) {
        nlohmann::json j;
        j["total"] = rooms.size();
        j["rooms"] = nlohmann::json::array();
        for (auto& r : rooms) {

            std::string id = r.value("room_id", "");
            j["rooms"].push_back({
                {"room_id", id},
                {"name", r.value("name", id)},
                {"messages", dbi.getEventCount(id)}
            });
        }
        std::cout << j.dump() << std::endl;
        return 0;
    }

    if (rooms.empty()) {
        std::cout << "No rooms in cache." << std::endl;
        std::cout << "  For demo:   matrixcli demo    (start demo server, then try again)" << std::endl;
        std::cout << "  For real:   matrixcli login   (login to Matrix first)" << std::endl;
        return 0;
    }
    for (auto& r : rooms) {
        std::string name = r.value("name", r.value("room_id", "?"));
        int msgs = dbi.getEventCount(r.value("room_id", ""));
        std::cout << name << "  [" << msgs << " msgs]" << std::endl;
    }
    return 0;
}

int cmdView(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    if (args.positional.empty()) {
        std::cerr << "Usage: matrixcli view <room> [limit] [--thread event_id] [--before eid] [--from eid]\n"
                     "       [--senders @u:h,@u2:h] [--hide @u:h] [--replies N|off] [--no-replies]\n"
                     "       [--no-filter] [--json] [--expand] [--verbose] [--ts]"
                  << std::endl;
        return 1;
    }
    std::string query = args.positional[0];
    int limit = 20;
    if (args.positional.size() >= 2 && !args.positional[1].starts_with("--")) {
        std::string lv = args.positional[1];
        limit = (lv == "all" || lv == "0") ? -1 : std::stoi(lv);
    }
    auto lm = args.options.find("limit");
    if (lm != args.options.end()) {
        std::string lv = lm->second;
        limit = (lv == "all" || lv == "0") ? -1 : std::stoi(lv);
    }

    std::string thread_root;
    auto tr_it = args.options.find("thread");
    if (tr_it != args.options.end()) thread_root = tr_it->second;

    std::string before;
    auto bf_it = args.options.find("before");
    if (bf_it != args.options.end()) before = bf_it->second;

    std::string from;
    auto fm_it = args.options.find("from");
    if (fm_it != args.options.end()) from = fm_it->second;

    // Temporary sender filters (this invocation only)
    auto splitMxids = [](const std::string& csv) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : csv) {
            if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    std::vector<std::string> tmp_senders, tmp_hide;
    auto sn_it = args.options.find("senders");
    if (sn_it != args.options.end()) tmp_senders = splitMxids(sn_it->second);
    auto hd_it = args.options.find("hide");
    if (hd_it != args.options.end()) tmp_hide = splitMxids(hd_it->second);
    bool no_filter = args.options.count("no-filter");

    // Reply chains (element-web style): show the replied-to message(s), and one
    // level further if that message is itself a reply. Depth defaults to 3.
    bool show_replies = true;
    int reply_depth = 3;
    if (args.options.count("no-replies")) show_replies = false;
    auto rp_it = args.options.find("replies");
    if (rp_it != args.options.end()) {
        std::string rv = rp_it->second;
        if (rv == "0" || rv == "off" || rv == "no") show_replies = false;
        else { try { reply_depth = std::stoi(rv); if (reply_depth <= 0) show_replies = false; } catch (...) {} }
    }

    bool verbose = args.options.count("verbose") || args.options.count("ids");
    bool show_ts = args.options.count("ts") || args.options.count("time");
    bool debug = args.options.count("debug") || args.options.count("raw");
    bool json_out = args.options.count("json");
    bool expand = args.options.count("expand") || args.options.count("full");

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) { std::cerr << "Cannot open database" << std::endl; return 1; }

    std::string room_id;
    std::string room_name;
    bool found = false;
    auto rooms = dbi.listRooms();
    for (auto& r : rooms) {
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        if (id == query || name == query || name.find(query) == 0) {
            room_id = id;
            room_name = name;
            found = true;
            break;
        }
    }
    if (room_id.empty()) { room_id = query; room_name = query; }

    auto events = dbi.getEvents(room_id, limit > 0 ? limit : 999999, before, from);

    // Effective filters: temporary flags (--senders/--hide) take precedence over
    // permanent config filters (per-room, then global). --no-filter disables all.
    Config::instance().load("config.json");
    std::vector<std::string> senders_filter = tmp_senders;
    std::vector<std::string> hide_filter = tmp_hide;
    if (!no_filter) {
        nlohmann::json flt = Config::instance().filters();
        auto applyJsonList = [](const nlohmann::json& j, const std::string& key, std::vector<std::string>& out) {
            if (j.is_object() && j.contains(key) && j[key].is_array())
                for (auto& v : j[key]) if (v.is_string()) out.push_back(v.get<std::string>());
        };
        nlohmann::json room_flt;
        if (flt.contains("rooms") && flt["rooms"].is_object() && flt["rooms"].contains(room_id))
            room_flt = flt["rooms"][room_id];
        if (senders_filter.empty()) applyJsonList(room_flt, "senders", senders_filter);
        if (senders_filter.empty()) applyJsonList(flt, "senders", senders_filter);
        applyJsonList(room_flt, "hide", hide_filter);
        applyJsonList(flt, "hide", hide_filter);
    }
    if (!senders_filter.empty() || !hide_filter.empty()) {
        events.erase(std::remove_if(events.begin(), events.end(), [&](const matrix::Event& e) {
            if (!senders_filter.empty()) {
                bool ok = false;
                for (auto& s : senders_filter) if (e.sender == s) { ok = true; break; }
                if (!ok) return true;
            }
            for (auto& h : hide_filter) if (e.sender == h) return true;
            return false;
        }), events.end());
    }

    // JSON output mode (pipe-friendly: pure JSON on stdout, chronological order)
    if (json_out) {
        nlohmann::json j;
        j["room_id"] = room_id;
        j["known"] = found;
        j["messages"] = nlohmann::json::array();
        if (from.empty()) std::reverse(events.begin(), events.end());
        for (auto& ev : events) {
            nlohmann::json m;
            m["event_id"] = ev.event_id;
            m["sender"] = ev.sender;
            m["type"] = ev.type;
            m["msgtype"] = ev.content.value("msgtype", "m.text");
            m["body"] = ev.content.value("body", "");
            m["ts"] = ev.origin_server_ts;
            m["reply_to"] = nlohmann::json::array();
            if (ev.content.contains("m.relates_to") && ev.content["m.relates_to"].contains("m.in_reply_to")) {
                std::string cur = ev.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
                for (int lvl = 0; !cur.empty() && lvl < reply_depth; ++lvl) {
                    matrix::Event anc;
                    if (!dbi.getEventById(cur, anc)) break;
                    nlohmann::json r;
                    r["event_id"] = anc.event_id;
                    r["sender"] = anc.sender;
                    r["body"] = anc.content.value("body", "");
                    m["reply_to"].push_back(r);
                    cur.clear();
                    if (anc.content.contains("m.relates_to") && anc.content["m.relates_to"].contains("m.in_reply_to"))
                        cur = anc.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
                }
            }
            j["messages"].push_back(m);
        }
        std::cout << j.dump() << std::endl;
        return 0;
    }

    if (events.empty()) {
        if (!before.empty()) std::cout << "(no older messages)" << std::endl;
        else std::cout << "(no messages in cache)" << std::endl;
        return 0;
    }

    if (from.empty()) std::reverse(events.begin(), events.end());

    // Human-mode header
    if (!thread_root.empty()) std::cout << "=== " << room_name << " / thread " << thread_root << " ===" << std::endl;
    else std::cout << "=== " << room_name << " (" << room_id << ") ===" << std::endl;

    // Show pagination hint
    bool has_newer, has_older;
    if (!from.empty()) {
        has_older = true;                              // history before the anchor exists
        has_newer = (int)events.size() >= limit;       // window full -> more newer
    } else if (!before.empty()) {
        has_newer = true;
        has_older = (int)events.size() >= limit;
    } else {
        has_newer = false;
        has_older = (int)events.size() >= limit;
    }

    // Message grouping
    std::string prev_sender;

    if (has_newer || has_older) {
        std::cout << "── ";
        if (has_newer) std::cout << "view --from " << events.back().event_id << " (newer)  ";
        if (has_older) std::cout << "view --before " << events.front().event_id << " (older)";
        std::cout << " ──" << std::endl;
    }

    for (auto& ev : events) {
        // Filter to thread if requested
        bool in_thread = false;
        if (!thread_root.empty()) {
            if (ev.content.contains("m.relates_to") &&
                ev.content["m.relates_to"].value("rel_type", "") == "m.thread" &&
                ev.content["m.relates_to"].value("event_id", "") == thread_root) {
                in_thread = true;
            } else if (ev.event_id != thread_root) {
                continue;
            }
        }

        std::string body = ev.content.value("body", "(no body)");
        // element-web style: strip the fallback "> quote" block from reply bodies
        // (the quote is rendered as reply context instead).
        auto stripFallbackQuote = [](std::string& s) {
            if (s.compare(0, 2, "> ") != 0) return;
            size_t start = 0;
            while (start < s.size()) {
                if (s.compare(start, 2, "> ") == 0) {
                    auto nl = s.find('\n', start);
                    if (nl == std::string::npos) { start = s.size(); break; }
                    start = nl + 1;
                } else break;
            }
            while (start < s.size() && (s[start] == '\n' || s[start] == ' ')) start++;
            s = start >= s.size() ? "" : s.substr(start);
        };
        if (show_replies && ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].contains("m.in_reply_to")) {
            stripFallbackQuote(body);
        }
        if (!expand && body.size() > 120) body = body.substr(0, 120) + "...";

        // Basic markdown → ANSI
        std::string md_body;
        for (size_t i = 0; i < body.size(); i++) {
            if (body[i] == '*' && i+1 < body.size() && body[i+1] == '*') {
                i += 2; md_body += ANSI_BOLD;
                while (i < body.size() && !(body[i] == '*' && i+1 < body.size() && body[i+1] == '*'))
                    md_body += body[i++];
                md_body += ANSI_RESET;
                if (i+1 < body.size()) i++;
                continue;
            }
            if (body[i] == '*' && i > 0 && body[i-1] == ' ') {
                i++; md_body += ANSI_ITALIC;
                while (i < body.size() && body[i] != '*') md_body += body[i++];
                md_body += ANSI_RESET;
                continue;
            }
            if (body[i] == '`') {
                i++; md_body += ANSI_DIM;
                while (i < body.size() && body[i] != '`') md_body += body[i++];
                md_body += ANSI_RESET;
                continue;
            }
            md_body += body[i];
        }
        std::string sender = ev.sender;
        std::string sender_name = util::userIdToName(sender);

        std::string ts_str;
        if (show_ts) ts_str = " " + relativeTime(ev.origin_server_ts);

        // Member events (join/leave/invite)
        std::string member_line;
        if (ev.type == "m.room.member" && ev.content.contains("membership")) {
            std::string membership = ev.content["membership"].get<std::string>();
            std::string displayname = ev.content.value("displayname", ev.state_key.empty() ? ev.sender : ev.state_key);
            if (displayname.starts_with("@")) displayname = displayname.substr(1);
            if (membership == "join") member_line = "→ " + displayname + " joined";
            else if (membership == "leave") member_line = "← " + displayname + " left";
            else if (membership == "invite") member_line = "✉ " + displayname + " invited";
            else if (membership == "ban") member_line = "⛔ " + displayname + " banned";
            else if (membership == "knock") member_line = "✊ " + displayname + " knocked";
        }

        if (!member_line.empty()) {
            std::cout << "  " ANSI_DIM "-- " << member_line << " --" ANSI_RESET << ts_str << std::endl;
            if (debug) std::cout << ANSI_GRAY "       id:" << ev.event_id << " state_key:" << ev.state_key << ANSI_RESET << std::endl;
            continue;
        }

        // Day separator
        static int64_t last_day = 0;
        time_t msg_t = ev.origin_server_ts / 1000;
        struct tm msg_tm;
        localtime_r(&msg_t, &msg_tm);
        msg_tm.tm_hour = 0; msg_tm.tm_min = 0; msg_tm.tm_sec = 0;
        int64_t msg_day = mktime(&msg_tm);
        if (msg_day != last_day && msg_day > 0) {
            last_day = msg_day;
            std::cout << std::endl << "  " ANSI_BOLD ANSI_CYAN << daySeparator(ev.origin_server_ts) << ANSI_RESET << std::endl << std::endl;
        }
        std::string prefix;
        if (ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].value("rel_type", "") == "m.thread") {
            prefix = "↳ ";
        }

        // Count thread replies
        int reply_count = 0;
        for (auto& other : events) {
            if (other.content.contains("m.relates_to") &&
                other.content["m.relates_to"].value("rel_type", "") == "m.thread" &&
                other.content["m.relates_to"].value("event_id", "") == ev.event_id) {
                reply_count++;
            }
        }

        // Reply context — element-web style multilevel chain: show the
        // replied-to message(s), one level deeper if those are replies too.
        if (show_replies && ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].contains("m.in_reply_to")) {
            std::string cur = ev.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
            for (int lvl = 0; !cur.empty() && lvl < reply_depth; ++lvl) {
                matrix::Event anc;
                if (!dbi.getEventById(cur, anc)) break;
                if (anc.type == "m.room.message" || anc.type == "m.text" || anc.type == "m.emote") {
                    std::string abody = anc.content.value("body", "");
                    stripFallbackQuote(abody);   // ancestors may be replies themselves
                    if (!expand && abody.size() > 100) abody = abody.substr(0, 100) + "...";
                    std::cout << "    " << std::string(lvl * 2, ' ') << ANSI_DIM "↱ "
                              << util::userIdToName(anc.sender) << ": " << abody << ANSI_RESET << std::endl;
                }
                cur.clear();
                if (anc.content.contains("m.relates_to") && anc.content["m.relates_to"].contains("m.in_reply_to"))
                    cur = anc.content["m.relates_to"]["m.in_reply_to"].value("event_id", "");
            }
        }

        // Link detection
        std::string link = extractLink(body);
        if (!link.empty() && !show_ts) {
            if (body.size() > 80) body = body.substr(0, 77) + "...";
        }

        std::string reply_str;
        if (reply_count > 0) reply_str = " [" + std::to_string(reply_count) + " replies]";

        // Message grouping: collapse sender if same as previous
        if (ev.sender == prev_sender && !prev_sender.empty()) {
            std::string indent(sender_name.size() + 3, ' ');
            std::cout << indent << prefix << md_body << reply_str;
        } else {
            prev_sender = ev.sender;
            std::cout << "  " << prefix << ansiUser(ev.sender, "[" + sender_name + "]") << ts_str << " " << md_body << reply_str;
        }

        // Show replied-to body if available
        if (ev.content.contains("m.relates_to")) {
            auto& rel = ev.content["m.relates_to"];
            if (rel.value("rel_type", "") == "m.in_reply_to" && ev.content.contains("m.new_content")) {
                std::string old_body = ev.content["m.new_content"].value("body", "");
                if (!old_body.empty())
                    std::cout << "\n" ANSI_GRAY "       ↪ \"" << old_body.substr(0, 60) << (old_body.size() > 60 ? "..." : "") << "\"" ANSI_RESET;
            }
        }
        if (verbose) {
            std::cout << "\n" ANSI_GRAY "       id:" << ev.event_id;
            if (!ev.redacts.empty()) std::cout << " redacts:" << ev.redacts;
            if (!ev.state_key.empty()) std::cout << " state_key:" << ev.state_key;
            std::cout << ANSI_RESET;
        }
        if (debug) {
            std::cout << "\n" ANSI_DIM "       raw:" << ev.content.dump() << ANSI_RESET;
        }
        std::cout << std::endl;
    }
    return 0;
}


// ---- Attach a file (media upload + send) ----
// matrixcli attach <room> <file> [--caption text]
// Plain rooms: upload + m.image/m.file/m.audio message. Encrypted rooms:
// the file is AES-CTR-encrypted client-side and sent as the m.encrypted
// "file" block (Element-compatible). Determines msgtype from the extension.
namespace matrixcli {
int cmdAttachFile(const cli::Args& args) {
    using namespace matrixcli;
    if (args.positional.size() < 2) {
        std::cerr << "Usage: matrixcli attach <room> <file> [--caption text] [--thread event_id]" << std::endl;
        return 1;
    }
    std::string query = args.positional[0];
    std::string path = args.positional[1];
    std::string caption = args.options.count("caption") ? args.options.at("caption") : "";
    std::string thread_root = args.options.count("thread") ? args.options.at("thread") : "";

    // Read the file.
    std::ifstream fin(path, std::ios::binary);
    if (!fin) { std::cerr << "Cannot open file: " << path << std::endl; return 1; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(fin)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) { std::cerr << "Empty file: " << path << std::endl; return 1; }

    // Extension -> msgtype + content type.
    std::string fn = path;
    auto slash = fn.find_last_of('/');
    if (slash != std::string::npos) fn = fn.substr(slash + 1);
    auto dot = fn.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : fn.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(::tolower(c));
    std::string mt, ct;
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
        ext == "webp" || ext == "bmp" || ext == "svg") {
        mt = "m.image";
        ct = ext == "jpg" ? "image/jpeg" : "image/" + ext;
    } else if (ext == "mp4" || ext == "webm" || ext == "mov" || ext == "mkv") {
        mt = "m.video";
        ct = "video/" + ext;
    } else if (ext == "mp3" || ext == "ogg" || ext == "wav" || ext == "flac" ||
               ext == "opus" || ext == "m4a") {
        mt = "m.audio";
        ct = ext == "m4a" ? "audio/mp4" : "audio/" + ext;
    } else {
        mt = "m.file";
        ct = "application/octet-stream";
    }
    std::string bodyName = caption.empty() ? fn : caption;

    if (!pcore::init() || !pcore::loadSavedSession()) {
        std::cerr << "Not logged in. Run 'matrixcli login' first." << std::endl;
        return 1;
    }
    auto& core = pcore::core();
    std::string bootNote = pcore::bootstrap();
    if (!bootNote.empty()) { std::cerr << "Warning: " << bootNote << std::endl; return 1; }

    std::string room_id = query;
    {
        db::Database dbi;
        if (dbi.open("matrixcli.db")) {
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == query || name == query || name.find(query) == 0 ||
                    id.find(query) != std::string::npos) {
                    room_id = id;
                    break;
                }
            }
        }
    }
    auto client = core.client;
    std::string hs = client->account().homeserverUrl;
    std::string token = client->account().accessToken;

    bool encrypted = client->isRoomEncrypted(room_id);

    // Live upload progress on stderr (the callback runs on the curl thread).
    int lastPct = -1;
    bool progressDone = false;
    progressive::desktop::setHttpProgressCallback(
        [&lastPct, &progressDone](int64_t ul, int64_t ulTotal,
                                  int64_t, int64_t) {
            if (ulTotal > 0 && !progressDone) {
                int pct = static_cast<int>(ul * 100 / ulTotal);
                if (pct != lastPct) {
                    lastPct = pct;
                    std::fprintf(stderr, "\rupload: %d%% (%lld / %lld bytes)%s",
                                 pct, static_cast<long long>(ul),
                                 static_cast<long long>(ulTotal),
                                 pct >= 100 ? "\n" : "   ");
                    std::fflush(stderr);
                }
                if (pct >= 100) progressDone = true;
            }
        });
    auto finishProgress = [&progressDone]() {
        if (!progressDone) std::fprintf(stderr, "\n");
        progressive::desktop::setHttpProgressCallback({});
        progressDone = true;
    };

    auto up = client->uploadMedia(bytes, fn, ct);
    finishProgress();
    if (!up.ok) {
        std::cerr << "Upload failed: " << up.error.message << std::endl;
        return 1;
    }
    std::string mxc = up.data;

    if (!encrypted) {
        std::ostringstream content;
        content << "{\"msgtype\":\"" << mt << "\",\"body\":\"" << bodyName
                << "\",\"filename\":\"" << fn << "\",\"url\":\"" << mxc << "\"";
        if (!thread_root.empty()) {
            content << ",\"m.relates_to\":{\"rel_type\":\"m.thread\",\"event_id\":\""
                    << thread_root << "\"}";
        }
        content << "}";
        auto r = client->sendMessageEvent(room_id, "m.room.message", content.str());
        if (!r.ok) { std::cerr << "Send failed: " << r.error.message << std::endl; return 1; }
        std::cout << "Sent " << mt << " to " << room_id
                  << (thread_root.empty() ? "" : " (thread " + thread_root + ")")
                  << " (mxc " << mxc << ")" << std::endl;
        return 0;
    }

    // Encrypted room: AES-CTR the file + the Element "file" block.
    std::string encKey, encIv;
    if (!progressive::desktop::generateMediaKeyIv(encKey, encIv)) {
        std::cerr << "Media key generation failed" << std::endl;
        return 1;
    }
    std::vector<uint8_t> encBytes = progressive::desktop::aesCtrCrypt(bytes, encKey, encIv);
    if (encBytes.empty()) { std::cerr << "Media encryption failed" << std::endl; return 1; }
    std::string encSha = progressive::desktop::sha256Base64(bytes);
    int encLastPct = -1;
    bool encDone = false;
    progressive::desktop::setHttpProgressCallback(
        [&encLastPct, &encDone](int64_t ul, int64_t ulTotal, int64_t, int64_t) {
            if (ulTotal > 0 && !encDone) {
                int pct = static_cast<int>(ul * 100 / ulTotal);
                if (pct != encLastPct) {
                    encLastPct = pct;
                    std::fprintf(stderr, "\rupload: %d%% (%lld / %lld bytes)%s",
                                 pct, static_cast<long long>(ul),
                                 static_cast<long long>(ulTotal),
                                 pct >= 100 ? "\n" : "   ");
                    std::fflush(stderr);
                }
                if (pct >= 100) encDone = true;
            }
        });
    auto upEnc = client->uploadMedia(encBytes, fn, ct);
    if (!encDone) std::fprintf(stderr, "\n");
    progressive::desktop::setHttpProgressCallback({});
    if (!upEnc.ok) {
        std::cerr << "Upload failed: " << upEnc.error.message << std::endl;
        return 1;
    }
    std::ostringstream fbody;
    fbody << "{\"msgtype\":\"" << mt << "\",\"body\":\"" << bodyName
          << "\",\"filename\":\"" << fn << "\""
          << ",\"file\":{\"url\":\"" << upEnc.data << "\",\"key\":\"" << encKey
          << "\",\"iv\":\"" << encIv << "\",\"hashes\":{\"sha256\":\"" << encSha
          << "\"},\"v\":\"v2\",\"mimetype\":\"" << ct << "\"}"
          << ",\"info\":{\"mimetype\":\"" << ct << "\"}}";
    std::string fbodyStr = fbody.str();
    if (!thread_root.empty()) {
        // Insert the thread relation into the content object (before its close).
        std::string rel = ",\"m.relates_to\":{\"rel_type\":\"m.thread\",\"event_id\":\""
                        + thread_root + "\"}";
        size_t pos = fbodyStr.rfind('}');
        if (pos != std::string::npos) fbodyStr.insert(pos, rel);
    }
    std::string inner = "{\"type\":\"m.room.message\",\"content\":" + fbodyStr +
                        ",\"room_id\":\"" + room_id + "\"}";
    auto* dec = core.sync->decryptor();
    std::string sessId = dec->getOrCreateOutboundSession(room_id);
    if (sessId.empty()) {
        std::cerr << "Could not create the outbound megolm session" << std::endl;
        return 1;
    }
    std::string enc = dec->encryptMessage(room_id, client->account().deviceId, inner);
    if (enc.empty()) { std::cerr << "Encryption failed" << std::endl; return 1; }
    if (!dec->roomKeyShared(room_id)) {
        auto members = client->getRoomMembers(room_id);
        std::vector<std::string> memberIds;
        if (members.ok) {
            try {
                auto j = nlohmann::json::parse(members.data);
                for (auto& [uid, info] : j["chunk"].items()) {
                    (void)info;
                    memberIds.push_back(uid);
                }
            } catch (...) {}
        }
        bool shared = dec->shareRoomKey(room_id, memberIds,
                                        client->account().userId,
                                        client->account().deviceId, hs, token);
        if (!shared) {
            std::cerr << "Warning: room key share failed — the receiver may not "
                         "decrypt this file yet." << std::endl;
        }
        dec->markRoomKeyShared(room_id);
    }
    auto r = client->sendEncryptedEvent(room_id, enc, "txn" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    if (!r.ok) { std::cerr << "Send failed: " << r.error.message << std::endl; return 1; }
    std::cout << "Sent encrypted " << mt << " to " << room_id << std::endl;
    return 0;
}
} // namespace matrixcli

int cmdSendMsg(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    if (args.positional.size() < 2) {
        std::cerr << "Usage: matrixcli send <room> [--thread event_id] <message>" << std::endl;
        return 1;
    }
    std::string query = args.positional[0];
    std::string thread_root = args.options.count("thread") ? args.options.at("thread") : "";
    std::string body;
    for (size_t i = 1; i < args.positional.size(); i++) {
        if (i > 1) body += " "; body += args.positional[i];
    }

    bool json_out = args.options.count("json");

    // Vendored desktop core path (preferred). Thread replies use the core
    // too (thread relation added by SyncEngine::sendMessage).
    if (pcore::init() && pcore::loadSavedSession()) {
        auto& core = pcore::core();
        // E2EE must be initialized BEFORE sending: an uninitialized Olm
        // account has an all-A identity and every encrypted message is
        // undecryptable for everyone (seen live). Mirrors cmdSync.
        std::string bootNote = pcore::bootstrap();
        if (!bootNote.empty()) {
            std::cerr << "Warning: " << bootNote << std::endl;
            return 1;
        }
        std::string room_id = query;
        db::Database dbi;
        if (dbi.open("matrixcli.db")) {
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == query || name == query || name.find(query) == 0) { room_id = id; break; }
            }
        }
        // SyncEngine::sendMessage auto-encrypts encrypted rooms (share
        // room key + m.room.encrypted); plain rooms go out as-is. The old
        // plain sendMessage leaked plaintext into encrypted rooms. Thread
        // replies use the core too now (the legacy client path was broken:
        // 'Not logged in' — thread_root previously forced the legacy client).
        auto r = core.sync ? core.sync->sendMessage(room_id, body, "m.text", thread_root)
                           : core.client->sendMessage(room_id, body, "m.text");
        if (!r.ok) {
            std::string err = r.error.message.empty() ? "send failed" : r.error.message;
            std::cerr << "Send failed: " << err << std::endl;
            return 1;
        }
        if (json_out) {
            nlohmann::json j;
            j["event_id"] = r.data;
            j["room_id"] = room_id;
            std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Sent [" << r.data << "]" << std::endl;
        }
        return 0;
    }

    Config::instance().load("config.json");
    matrix::Client client;

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;
    auto acc = dbi.loadAccount();
    if (!acc.is_logged_in()) {
        std::cerr << "Not logged in. Run 'matrixcli login' first." << std::endl;
        return 1;
    }
    client.setHomeserverURL(acc.homeserver_url);
    client.setAccessToken(acc.access_token);
    client.setDatabase(&dbi);

    std::string room_id = query;
    auto rooms = dbi.listRooms();
    for (auto& r : rooms) {
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        if (id == query || name == query || name.find(query) == 0) {
            room_id = id;
            break;
        }
    }

    try {
        std::string event_id;
        if (!thread_root.empty()) {
            event_id = client.sendThreadReply(room_id, thread_root, body);
        } else {
            event_id = client.sendTextMessage(room_id, body);
        }
        if (args.options.count("json")) {
            nlohmann::json j;
            j["event_id"] = event_id;
            j["room_id"] = room_id;
            std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Sent [" << event_id << "]" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Send failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

int cmdSearch(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    if (args.positional.empty()) {
        std::cerr << "Usage: matrixcli search <query> [--limit N|all]" << std::endl;
        return 1;
    }
    std::string query = args.positional[0];
    int limit = -1; // -1 = unlimited
    auto lm = args.options.find("limit");
    if (lm != args.options.end()) {
        std::string lv = lm->second;
        limit = (lv == "all" || lv == "0") ? -1 : std::stoi(lv);
    }

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;

    auto results = dbi.search(query, limit > 0 ? limit : 999999);
    if (results.empty()) {
        std::cout << "No results for: " << query << std::endl;
        std::cout << "(Indexed during sync. Start: matrixcli serve, then sync populates FTS)" << std::endl;
        return 0;
    }
    std::cout << results.size() << " results for \"" << query << "\":" << std::endl;
    for (auto& r : results) {
        std::string sender = r.value("sender", "?");
        auto at = sender.find(':');
        if (at != std::string::npos && sender.starts_with("@")) sender = sender.substr(1, at - 1);
        std::string room = r.value("room_name", r.value("room_id", "?"));
        if (room.starts_with("#")) room = room.substr(1);
        std::string body = r.value("content", nlohmann::json::object()).value("body", "(no body)");
        if (body.size() > 100) body = body.substr(0, 100) + "...";
        std::cout << "  #" << room << "  [" << sender << "] " << body << std::endl;
    }
    return 0;
}

int cmdConfig(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    const std::string path = "matrixcli.toml";

#ifndef BUILD_TUI
    (void)args; (void)path;
    std::cerr << "TUI config is unavailable — this binary was built without the TUI (BUILD_TUI=OFF)" << std::endl;
    return 1;
#else
    if (args.options.count("set") && args.positional.size() >= 1) {
        tui::TUIConfig cfg = tui::TUIConfig::load(path);
        std::string key = args.options.at("set");
        std::string val = args.positional[0];
        if (key == "show_timestamps") cfg.show_timestamps = (val == "1" || val == "true" || val == "on");
        else if (key == "compact") cfg.compact_mode = (val == "1" || val == "true" || val == "on");
        else if (key == "sound") cfg.notification_sound = (val == "1" || val == "true" || val == "on");
        else if (key == "room_width") cfg.room_list_width = std::stoi(val);
        else if (key == "max_messages") cfg.max_messages = std::stoi(val);
        else if (key == "date_format") cfg.date_format = val;
        else { std::cerr << "Unknown key: " << key << std::endl; return 1; }
        cfg.save(path);
        std::cout << "Saved " << key << " = " << val << std::endl;
    } else if (args.options.count("get")) {
        tui::TUIConfig cfg = tui::TUIConfig::load(path);
        std::string key = args.options.at("get");
        if (key == "show_timestamps") std::cout << cfg.show_timestamps << std::endl;
        else if (key == "compact") std::cout << cfg.compact_mode << std::endl;
        else if (key == "sound") std::cout << cfg.notification_sound << std::endl;
        else if (key == "room_width") std::cout << cfg.room_list_width << std::endl;
        else if (key == "max_messages") std::cout << cfg.max_messages << std::endl;
        else if (key == "date_format") std::cout << cfg.date_format << std::endl;
        else { std::cerr << "Unknown: " << key << std::endl; return 1; }
    } else {
        tui::TUIConfig cfg = tui::TUIConfig::load(path);
        std::cout << "show_timestamps = " << cfg.show_timestamps << std::endl;
        std::cout << "compact         = " << cfg.compact_mode << std::endl;
        std::cout << "sound           = " << cfg.notification_sound << std::endl;
        std::cout << "room_width      = " << cfg.room_list_width << std::endl;
        std::cout << "max_messages    = " << cfg.max_messages << std::endl;
        std::cout << "date_format     = " << cfg.date_format << std::endl;
        std::cout << "\nSet:  matrixcli config --set key value" << std::endl;
    }
    return 0;
#endif
}

// Populate the offline demo database (rooms + messages + reply chain).
// Shared by `demo populate` and the interactive `demo` REPL.
static int populateDemoData(matrixcli::db::Database& dbi) {
    using namespace matrixcli;

    // Rebuild the demo from scratch: drop the demo-local rooms and their
    // events so re-running 'demo populate' always yields the CURRENT demo
    // (new rooms/messages/images/threads included).
    {
        auto existing = dbi.listRooms();
        for (const auto& r : existing) {
            std::string id = r.value("room_id", "");
            if (id.find(":demo.local") != std::string::npos) {
                dbi.clearRoom(id);
            }
        }
    }

    struct { const char* id; const char* name; const char* topic; int members; } rooms[] = {
        {"!general:demo.local","#general","General discussion",42},
        {"!dev:demo.local","#dev","Development chat",15},
        {"!random:demo.local","#random","Random stuff",28},
        {"!design:demo.local","#design","UI & design talk",9},
        {"!music:demo.local","#music","Music sharing",23},
        {"!games:demo.local","#games","Games & esports",37},
        {"!science:demo.local","#science","Science news",18},
        {"!offtopic:demo.local","#offtopic","Off-topic banter",54},
        {"!announce:demo.local","#announcements","Official announcements",120},
        {"!help:demo.local","#help","Support chat",31},
        {"!linux:demo.local","#linux","Linux & FOSS",66},
        {"!crypto:demo.local","#crypto","Crypto & Web3",42},
        {"!photography:demo.local","#photography","Camera talk",19},
        {"!travel:demo.local","#travel","Travel stories",27},
        {"!food:demo.local","#food","Cooking & recipes",35},
        {"!books:demo.local","#books","Reading club",14},
        {"!fitness:demo.local","#fitness","Workout logs",22},
        {"!movies:demo.local","#movies","Film & TV",48},
        {"!programming:demo.local","#programming","Code help",61},
        {"!rust:demo.local","#rust","Rust lang",33},
        {"!matrix:demo.local","#matrix","Matrix protocol",17},
        {"!meta:demo.local","#meta","About this demo",8},
        {"!sports:demo.local","#sports","Game day talk",41},
        {"!chess:demo.local","#chess","Chess & strategy",16},
        {"!retro-gaming:demo.local","#retro-gaming","Retro consoles",29},
        {"!hardware:demo.local","#hardware","PC building",58},
        {"!distro-talk:demo.local","#distro-talk","Distro hopping",36},
        {"!shell:demo.local","#shell","Shell scripting",44},
        {"!editors:demo.local","#editors","Editor wars",52},
        {"!git:demo.local","#git","Version control",48},
        {"!dotfiles:demo.local","#dotfiles","Dotfiles showoff",31},
        {"!selfhosting:demo.local","#selfhosting","Self-host your life",67},
        {"!homelab:demo.local","#homelab","Homelab corner",39},
        {"!security:demo.local","#security","Security & exploits",54},
        {"!privacy:demo.local","#privacy","Privacy tools",47},
        {"!networking:demo.local","#networking","Networks & routing",28},
        {"!databases:demo.local","#databases","SQL & NoSQL",33},
        {"!webdev:demo.local","#webdev","Web development",62},
        {"!frontend:demo.local","#frontend","Frontend craft",45},
        {"!backend:demo.local","#backend","Backend services",40},
        {"!ml:demo.local","#ml","Machine learning",57},
        {"!ai-art:demo.local","#ai-art","AI generated art",35},
        {"!astronomy:demo.local","#astronomy","Stars & space",26},
        {"!physics:demo.local","#physics","Physics chat",38},
        {"!chemistry:demo.local","#chemistry","Lab banter",21},
        {"!biology:demo.local","#biology","Life sciences",24},
        {"!math:demo.local","#math","Math problems",30},
        {"!history:demo.local","#history","History corner",43},
        {"!philosophy:demo.local","#philosophy","Deep thoughts",27},
        {"!languages:demo.local","#languages","Language learning",34},
        {"!writing:demo.local","#writing","Writing & prose",22},
        {"!poetry:demo.local","#poetry","Poetry corner",15},
        {"!art:demo.local","#art","Art & craft",33},
        {"!pixelart:demo.local","#pixelart","Pixel art",25},
        {"!music-production:demo.local","#music-production","Making beats",37},
        {"!synth:demo.local","#synth","Synthesizers",18},
        {"!jazz:demo.local","#jazz","Jazz lounge",20},
        {"!metal:demo.local","#metal","Metal heads",46},
        {"!classical:demo.local","#classical","Classical music",23},
        {"!techno:demo.local","#techno","Techno warehouse",32},
        {"!dnb:demo.local","#dnb","Drum & bass",28},
        {"!hiking:demo.local","#hiking","Trails & peaks",40},
        {"!camping:demo.local","#camping","Camping gear",26},
        {"!cycling:demo.local","#cycling","Bike rides",35},
        {"!running:demo.local","#running","Running club",44},
        {"!climbing:demo.local","#climbing","Climbing gym",19},
        {"!yoga:demo.local","#yoga","Yoga & stretch",17},
        {"!vegan:demo.local","#vegan","Vegan cooking",29},
        {"!baking:demo.local","#baking","Bread & pastry",31},
        {"!coffee:demo.local","#coffee","Coffee brewing",42},
        {"!tea:demo.local","#tea","Tea time",24},
        {"!beer:demo.local","#beer","Craft beer",38},
        {"!wine:demo.local","#wine","Wine cellar",13},
        {"!boardgames:demo.local","#boardgames","Board game night",27},
        {"!podcasts:demo.local","#podcasts","Podcast picks",21},
        {"!memes:demo.local","#memes","Memes & jokes",55},
        {"!diy:demo.local","#diy","DIY projects",36},
        {"!finance:demo.local","#finance","Personal finance",49},
        {"!dm_alice:demo.local","Alice","",2},
        {"!dm_bob:demo.local","Bob","",2},
        {"!dm_carol:demo.local","Carol","",2},
        {"!dm_dave:demo.local","Dave","",2},
    };
    for (auto& r : rooms) {
        nlohmann::json j;
        j["name"] = r.name; j["topic"] = r.topic; j["member_count"] = r.members;
        dbi.upsertRoom(j, r.id);
    }

    // Matrix origin_server_ts is milliseconds since epoch
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    struct { const char* room; const char* sender; const char* name; const char* body; } msgs[] = {
        {"!general:demo.local","@alice","Alice","Welcome! This is matrixcli — a terminal Matrix client."},
        {"!general:demo.local","@bob","Bob","Supports E2EE, SQLite cache, multi-format REST API."},
        {"!general:demo.local","@alice","Alice","Try: matrixcli tui, matrixcli view, matrixcli send"},
        {"!general:demo.local","@alice","Alice","Multiline demo message:\nline one\nline two\nline three"},
        {"!dev:demo.local","@charlie","Charlie","C++20, raw sockets + OpenSSL, no external HTTP libs."},
        {"!dev:demo.local","@alice","Alice","CMake build, 5 format renderers, full Matrix CS API."},
        {"!random:demo.local","@bob","Bob","Why did the dev quit? No arrays."},
        {"!dm_alice:demo.local","@alice","Alice","Hey! This is a private encrypted DM."},
        {"!dm_alice:demo.local","@you","You","Hi Alice! The ascii client is really nice."},
        {"!dm_alice:demo.local","@alice","Alice","Glad you like it — and the DMs work offline too."},
        {"!dm_bob:demo.local","@bob","Bob","Try matrixcli view \"!dm_bob:demo.local\""},
        {"!dm_bob:demo.local","@you","You","Will do — I sent you a file by the way."},
        {"!dm_bob:demo.local","@bob","Bob","Got it, report.pdf looks good."},
    };
    int64_t dayMs = 86400000;
    int day = 0;
    for (auto& m : msgs) {
        matrix::Event ev;
        ev.event_id = "$demo_" + std::to_string(ts);
        ev.room_id = m.room; ev.sender = m.sender;
        ev.type = "m.room.message";
        ev.content = {{"body", m.body}, {"msgtype", "m.text"}};
        ev.origin_server_ts = ts;
        dbi.insertEvent(ev);
        // Spread the history across 5 days (an "old" room): every other
        // message jumps back one day.
        if (day % 2 == 1) ts -= dayMs;
        ts -= 3600000;  // an hour between messages
        day++;
    }

    // Content for the extra rooms: mentions, urls, files, audio.
    {
        struct { const char* room; const char* sender; const char* body; } extra[] = {
            {"!design:demo.local", "@carol", "The ascii ui looks great, @you — nice work on the pipes."},
            {"!design:demo.local", "@dave", "Agreed! And the thread panel is super useful."},
            {"!design:demo.local", "@you", "Thanks! Check the design spec: https://matrix.org/docs/"},
            {"!design:demo.local", "@carol", "Also sent the mockups as files, see above."},
            {"!music:demo.local", "@erin", "New album out today 🎵 https://soundcloud.com/example"},
            {"!music:demo.local", "@you", "Listening now. The bass is great."},
            {"!music:demo.local", "@erin", "I'll upload the studio recording as an audio file."},
            {"!games:demo.local", "@frank", "Patch notes are live: https://matrix.org/blog/"},
            {"!games:demo.local", "@you", "Nice, the new map is huge. @frank up for a game tonight?"},
            {"!games:demo.local", "@frank", "Sure! I'll drop the invite file here."},
            {"!science:demo.local", "@grace", "The paper is out — abstract: https://arxiv.org/"},
            {"!science:demo.local", "@you", "Fascinating read, @grace. The figures are great."},
            {"!help:demo.local", "@you", "How do I reset my keys? Anyone?"},
            {"!help:demo.local", "@grace", "@you — Preferences → Reset device keys, then re-verify."},
            {"!help:demo.local", "@you", "Thanks @grace! It worked."},
            {"!offtopic:demo.local", "@erin", "Random question: how do you take your coffee?"},
            {"!offtopic:demo.local", "@dave", "Black, always. @frank is a latte guy."},
            {"!offtopic:demo.local", "@frank", "Latte supremacy!"},
            {"!announce:demo.local", "@alice", "Welcome to the community! Rules: https://matrix.org/"},
            {"!announce:demo.local", "@bob", "And the code of conduct is pinned in this room."},
            {"!dm_carol:demo.local", "@carol", "Hey @you! Want to review my ui sketches?"},
            {"!dm_carol:demo.local", "@you", "Sure! Send them over."},
            {"!dm_carol:demo.local", "@carol", "Here you go: https://example.com/sketch.png"},
            {"!dm_dave:demo.local", "@dave", "@you — the demo DMs are two-sided now!"},
            {"!dm_dave:demo.local", "@you", "I noticed, nice."},
            {"!dm_dave:demo.local", "@dave", "I'll send you an audio note to test playback."},
        };
        for (auto& m : extra) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.message";
            ev.content = {{"body", m.body}, {"msgtype", "m.text"}};
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            if (day % 2 == 1) ts -= dayMs;
            ts -= 3600000;
            day++;
        }
        struct { const char* room; const char* sender; const char* body; } extra2[] = {
            {"!linux:demo.local", "@dave", "My kernel finally boots. https://kernel.org/"},
            {"!linux:demo.local", "@you", "@dave nice! Which distro?"},
            {"!crypto:demo.local", "@frank", "Don't trust, verify. https://bitcoin.org/"},
            {"!photography:demo.local", "@carol", "Golden hour today, look at the light."},
            {"!photography:demo.local", "@you", "Great shot! Settings?"},
            {"!travel:demo.local", "@erin", "Just landed in Tokyo! https://japan.travel/"},
            {"!travel:demo.local", "@you", "Envious @erin! Send photos."},
            {"!food:demo.local", "@bob", "Tonight: ramen from scratch."},
            {"!food:demo.local", "@you", "Recipe? I need that in my life."},
            {"!books:demo.local", "@grace", "The manual is a masterpiece."},
            {"!fitness:demo.local", "@you", "Morning run done. 5k."},
            {"!movies:demo.local", "@alice", "Anyone seen the new sci-fi?"},
            {"!movies:demo.local", "@you", "Yes! The plot twist though."},
            {"!programming:demo.local", "@dave", "Segfault at line 42. Classic."},
            {"!programming:demo.local", "@you", "@dave use-after-free probably."},
            {"!rust:demo.local", "@frank", "Borrow checker saves the day again."},
            {"!rust:demo.local", "@you", "It compiles first try. @frank today was a good day."},
            {"!matrix:demo.local", "@alice", "The protocol spec is here: https://spec.matrix.org/"},
            {"!matrix:demo.local", "@you", "Reading it now, @alice."},
            {"!meta:demo.local", "@you", "This is the demo room, try everything here."},
        };
        for (auto& m : extra2) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.message";
            ev.content = {{"body", m.body}, {"msgtype", "m.text"}};
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            if (day % 2 == 1) ts -= dayMs;
            ts -= 3600000;
            day++;
        }

        struct { const char* room; const char* sender; const char* msgtype; const char* body; } extra3[] = {
            {"!sports:demo.local", "@alice", "m.text", "Big match tonight, who's watching?"},
            {"!chess:demo.local", "@bob", "m.text", "Puzzle of the day: white to move, mate in 3."},
            {"!retro-gaming:demo.local", "@alice", "m.text", "Just finished Zelda on the SNES emulator."},
            {"!hardware:demo.local", "@charlie", "m.file", "schematic-v2.pdf"},
            {"!distro-talk:demo.local", "@alice", "m.text", "Switched to Arch, btw. https://archlinux.org/"},
            {"!shell:demo.local", "@erin", "m.text", "TIL: ctrl+r searches history in reverse."},
            {"!editors:demo.local", "@alice", "m.text", "vim or neovim? The debate continues."},
            {"!git:demo.local", "@grace", "m.text", "@you rebase or merge?"},
            {"!dotfiles:demo.local", "@carol", "m.file", "notes-q3.pdf"},
            {"!selfhosting:demo.local", "@dave", "m.file", "hardware-review.pdf"},
            {"!homelab:demo.local", "@alice", "m.text", "Plex server finally up!"},
            {"!security:demo.local", "@carol", "m.text", "Never store plaintext passwords, folks."},
            {"!privacy:demo.local", "@alice", "m.text", "Worth a read: https://www.torproject.org/"},
            {"!networking:demo.local", "@erin", "m.text", "VLANs are the answer to 90% of my problems."},
            {"!databases:demo.local", "@erin", "m.file", "benchmark-results.pdf"},
            {"!webdev:demo.local", "@grace", "m.text", "The new CSS grid features are great."},
            {"!frontend:demo.local", "@alice", "m.text", "Rewrote the site with no JS. So fast now."},
            {"!backend:demo.local", "@bob", "m.text", "Latency down to 12ms after the rewrite."},
            {"!ml:demo.local", "@alice", "m.text", "Fine-tuned a small model today, results are neat."},
            {"!ai-art:demo.local", "@carol", "m.text", "Prompt: cyberpunk cat, 4k, cinematic lighting."},
            {"!astronomy:demo.local", "@alice", "m.poll", "Which planet is best for a new mission?"},
            {"!physics:demo.local", "@erin", "m.text", "Why is entropy always increasing?"},
            {"!chemistry:demo.local", "@alice", "m.text", "The coffee filter chromatogram works."},
            {"!biology:demo.local", "@grace", "m.text", "CRISPR news today is wild. https://en.wikipedia.org/wiki/CRISPR"},
            {"!math:demo.local", "@alice", "m.text", "Prove pi is irrational. Go. @you"},
            {"!history:demo.local", "@bob", "m.text", "The library of Alexandria keeps me up at night."},
            {"!philosophy:demo.local", "@alice", "m.text", "Is a broken clock right twice a day?"},
            {"!languages:demo.local", "@carol", "m.text", "Duolingo streak: 47 days. @you join me!"},
            {"!writing:demo.local", "@frank", "m.file", "meeting-notes.pdf"},
            {"!poetry:demo.local", "@erin", "m.text", "Roses are red, my terminal is green..."},
            {"!art:demo.local", "@alice", "m.text", "Sketching the market square this weekend."},
            {"!pixelart:demo.local", "@grace", "m.text", "16x16 sprite of a frog. Cute."},
            {"!music-production:demo.local", "@dave", "m.audio", "riff-idea.mp3"},
            {"!synth:demo.local", "@erin", "m.audio", "demo-track.mp3"},
            {"!jazz:demo.local", "@alice", "m.text", "Miles Davis tonight. Recommended: https://open.spotify.com/"},
            {"!metal:demo.local", "@frank", "m.audio", "voice-note.m4a"},
            {"!classical:demo.local", "@alice", "m.text", "Bach's cello suites are perfection."},
            {"!techno:demo.local", "@erin", "m.text", "Warehouse party this Saturday!"},
            {"!dnb:demo.local", "@alice", "m.text", "Drum and bass fills me with energy."},
            {"!hiking:demo.local", "@grace", "m.text", "Summited the ridge, view was unreal."},
            {"!camping:demo.local", "@grace", "m.file", "packing-list.pdf"},
            {"!cycling:demo.local", "@bob", "m.text", "100km ride done, legs are gone."},
            {"!running:demo.local", "@alice", "m.text", "New PB: 5k in 22:14!"},
            {"!climbing:demo.local", "@carol", "m.text", "Sent my first 7a today, @you should try it."},
            {"!yoga:demo.local", "@alice", "m.text", "Morning flow, 20 minutes, game changer."},
            {"!vegan:demo.local", "@erin", "m.text", "Tofu scramble recipe incoming."},
            {"!baking:demo.local", "@alice", "m.text", "Sourdough loaf #12, best one yet."},
            {"!coffee:demo.local", "@grace", "m.text", "Pour over > espresso. Fight me. https://en.wikipedia.org/wiki/Pour-over_coffee"},
            {"!tea:demo.local", "@alice", "m.text", "Aged oolong, notes of honey and stone fruit."},
            {"!beer:demo.local", "@alice", "m.poll", "Pilsner or IPA this Friday?"},
            {"!wine:demo.local", "@alice", "m.text", "Cork vs screwcap, discuss."},
            {"!boardgames:demo.local", "@alice", "m.poll", "Which game for Friday night?"},
            {"!podcasts:demo.local", "@grace", "m.audio", "podcast-clip.mp3"},
            {"!memes:demo.local", "@erin", "m.text", "me: I'll just fix one thing. 6 hours later:"},
            {"!diy:demo.local", "@alice", "m.text", "Built a desk from pallets, zero regrets."},
            {"!finance:demo.local", "@grace", "m.text", "Emergency fund first, then investments. @you"},
        };
        for (auto& m : extra3) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.message";
            if (!strcmp(m.msgtype, "m.file")) {
                ev.content = {{"msgtype", "m.file"}, {"body", m.body},
                              {"url", "mxc://demo.local/f_" + std::string(m.body)},
                              {"filename", m.body}, {"mimetype", "application/pdf"}};
            } else if (!strcmp(m.msgtype, "m.audio")) {
                ev.content = {{"msgtype", "m.audio"}, {"body", m.body},
                              {"url", "mxc://demo.local/a_" + std::string(m.body)},
                              {"filename", m.body}, {"mimetype", "audio/mpeg"}};
            } else if (!strcmp(m.msgtype, "m.poll")) {
                std::string pid = "$demo_" + std::to_string(ts);
                ev.content = {{"msgtype", "m.poll.start"},
                              {"question", {{"text", m.body}}},
                              {"answers", {{{"id", "a"}, {"text", "Option A"}},
                                           {{"id", "b"}, {"text", "Option B"}}}},
                              {"m.relates_to", {{"event_id", pid}, {"rel_type", "m.reference"}}}};
            } else {
                ev.content = {{"msgtype", "m.text"}, {"body", m.body}};
            }
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            if (day % 2 == 1) ts -= dayMs;
            ts -= 3600000;
            day++;
        }

        // A file + an audio in the design room.
        matrix::Event f;
        f.event_id = "$demo_" + std::to_string(ts);
        f.room_id = "!design:demo.local"; f.sender = "@carol";
        f.type = "m.room.message";
        f.content = {{"msgtype", "m.file"}, {"body", "mockups v2 final.zip"},
                     {"filename", "mockups v2 final.zip"}, {"url", "mxc://demo.local/mockups"},
                     {"info", {{"mimetype", "application/zip"}, {"size", 2048033}}}};
        f.origin_server_ts = ts;
        dbi.insertEvent(f);
        ts -= 3600000;
        matrix::Event a;
        a.event_id = "$demo_" + std::to_string(ts);
        a.room_id = "!music:demo.local"; a.sender = "@erin";
        a.type = "m.room.message";
        a.content = {{"msgtype", "m.audio"}, {"body", "studio-recording.ogg"},
                     {"filename", "studio-recording.ogg"}, {"url", "mxc://demo.local/rec"},
                     {"info", {{"mimetype", "audio/ogg"}, {"size", 240934}}}};
        a.origin_server_ts = ts;
        dbi.insertEvent(a);
        ts -= 3600000;
    }

    // An image message + reactions (the ui renders the card and the
    // "❤ 2" counts under the message).
    {
        std::string imgId = "$demo_" + std::to_string(ts);
        matrix::Event img;
        img.event_id = imgId;
        img.room_id = "!general:demo.local"; img.sender = "@bob";
        img.type = "m.room.message";
        img.content = {{"msgtype", "m.image"}, {"body", "sunset.png"},
                       {"url", "mxc://demo.local/sunset"},
                       {"info", {{"mimetype", "image/png"}, {"size", 204800},
                                 {"w", 1024}, {"h", 768}}}};
        img.origin_server_ts = ts;
        dbi.insertEvent(img);
        ts -= 60;

        struct { const char* key; } reacts[] = {{"\xe2\x9d\xa4\xef\xb8\x8f"},
                                                {"\xe2\x9d\xa4\xef\xb8\x8f"},
                                                {"\xf0\x9f\x91\x8d"}};
        for (auto& r : reacts) {
            matrix::Event re;
            re.event_id = "$demo_" + std::to_string(ts);
            re.room_id = "!general:demo.local"; re.sender = "@alice";
            re.type = "m.reaction";
            re.content = {{"m.relates_to",
                           {{"rel_type", "m.annotation"}, {"event_id", imgId},
                            {"key", r.key}}}};
            re.origin_server_ts = ts;
            dbi.insertEvent(re);
            ts -= 60;
        }
    }

    // A regular file + an audio message (the ui renders 📄 / 🎵 cards).
    {
        matrix::Event f;
        f.event_id = "$demo_" + std::to_string(ts);
        f.room_id = "!general:demo.local"; f.sender = "@charlie";
        f.type = "m.room.message";
        f.content = {{"msgtype", "m.file"}, {"body", "report.pdf"},
                     {"filename", "report.pdf"}, {"url", "mxc://demo.local/report"},
                     {"info", {{"mimetype", "application/pdf"}, {"size", 482033}}}};
        f.origin_server_ts = ts;
        dbi.insertEvent(f);
        ts -= 3600000;
        matrix::Event a;
        a.event_id = "$demo_" + std::to_string(ts);
        a.room_id = "!random:demo.local"; a.sender = "@bob";
        a.type = "m.room.message";
        a.content = {{"msgtype", "m.audio"}, {"body", "voice-note.ogg"},
                     {"filename", "voice-note.ogg"}, {"url", "mxc://demo.local/voice"},
                     {"info", {{"mimetype", "audio/ogg"}, {"size", 120934}}}};
        a.origin_server_ts = ts;
        dbi.insertEvent(a);
        ts -= 3600000;
    }

    // A poll (m.poll.start) + two responses.
    {
        std::string pollId = "$demo_" + std::to_string(ts);
        matrix::Event p;
        p.event_id = pollId;
        p.room_id = "!general:demo.local"; p.sender = "@alice";
        p.type = "m.room.message";
        p.content = {{"msgtype", "m.poll.start"},
                     {"question", {{"text", "Where should the meetup be?"}}},
                     {"answers", {{{"id", "a"}, {"text", "Park"}},
                                  {{"id", "b"}, {"text", "Cafe"}}}},
                     {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}}};
        p.origin_server_ts = ts;
        dbi.insertEvent(p);
        ts -= 3600000;
        struct { const char* sender; const char* vote; } votes[] = {
            {"@bob", "a"}, {"@charlie", "a"}, {"@you", "b"},
        };
        for (auto& v : votes) {
            matrix::Event r;
            r.event_id = "$demo_" + std::to_string(ts);
            r.room_id = "!general:demo.local"; r.sender = v.sender;
            r.type = "m.room.message";
            r.content = {{"msgtype", "m.poll.response"},
                         {"m.relates_to", {{"event_id", pollId}, {"rel_type", "m.reference"}}},
                         {"selections", {v.vote}}};
            r.origin_server_ts = ts;
            dbi.insertEvent(r);
            ts -= 3600000;
        }
    }

    // A redacted (deleted) message + the redaction event.
    {
        std::string doomedId = "$demo_" + std::to_string(ts);
        matrix::Event d;
        d.event_id = doomedId;
        d.room_id = "!random:demo.local"; d.sender = "@charlie";
        d.type = "m.room.message";
        d.content = {{"msgtype", "m.text"}, {"body", "this message will be deleted"}};
        d.origin_server_ts = ts;
        dbi.insertEvent(d);
        ts -= 3600000;
        matrix::Event red;
        red.event_id = "$demo_" + std::to_string(ts);
        red.room_id = "!random:demo.local"; red.sender = "@charlie";
        red.type = "m.room.redaction";
        red.redacts = doomedId;
        red.content = nlohmann::json::object();
        red.origin_server_ts = ts;
        dbi.insertEvent(red);
        ts -= 3600000;
    }

    // Power levels in the DMs: everybody is admin there (like Element).
    {
        matrix::Event pl;
        pl.event_id = "$demo_" + std::to_string(ts);
        pl.room_id = "!dm_alice:demo.local"; pl.sender = "@alice";
        pl.type = "m.room.power_levels";
        pl.content = {{"users", {{"@alice", 100}, {"@you", 100}}}};
        pl.origin_server_ts = ts;
        dbi.insertEvent(pl);
        ts -= 3600000;
        matrix::Event pl2;
        pl2.event_id = "$demo_" + std::to_string(ts);
        pl2.room_id = "!dm_bob:demo.local"; pl2.sender = "@bob";
        pl2.type = "m.room.power_levels";
        pl2.content = {{"users", {{"@bob", 100}, {"@you", 100}}}};
        pl2.origin_server_ts = ts;
        dbi.insertEvent(pl2);
        ts -= 3600000;
    }

    // Power levels: alice admin (100), bob mod (50), the rest members.
    {
        matrix::Event pl;
        pl.event_id = "$demo_" + std::to_string(ts);
        pl.room_id = "!general:demo.local"; pl.sender = "@alice";
        pl.type = "m.room.power_levels";
        pl.content = {{"users", {{"@alice", 100}, {"@bob", 50}, {"@charlie", 0}}}};
        pl.origin_server_ts = ts;
        dbi.insertEvent(pl);
        ts -= 3600000;
    }

    // A message with a permalink (matrix.to) — the ui renders it as a pill.
    {
        std::string welcomeId = "$demo_" + std::to_string(ts + 8 * 60);
        matrix::Event pl;
        pl.event_id = "$demo_" + std::to_string(ts);
        pl.room_id = "!general:demo.local"; pl.sender = "@bob";
        pl.type = "m.room.message";
        pl.content = {{"msgtype", "m.text"},
                      {"body", "Look at this: https://matrix.to/#/!general:demo.local/"
                               + welcomeId}};
        pl.origin_server_ts = ts;
        dbi.insertEvent(pl);
        ts -= 60;
    }

    // Membership events: joined/left rows (the ui renders them as system lines).
    {
        auto shortName = [](const char* s) {
            std::string out = s;
            if (!out.empty() && out[0] == '@') {
                auto colon = out.find(':');
                if (colon != std::string::npos) out = out.substr(1, colon - 1);
                else out = out.substr(1);
            }
            return out;
        };
        struct { const char* room; const char* sender; const char* ms; } mem[] = {
            {"!general:demo.local", "@bob", "join"},
            {"!general:demo.local", "@charlie", "join"},
            {"!random:demo.local", "@bob", "join"},
            {"!dev:demo.local", "@bob", "leave"},
            {"!design:demo.local", "@carol", "join"},
            {"!design:demo.local", "@dave", "join"},
            {"!music:demo.local", "@erin", "join"},
            {"!games:demo.local", "@frank", "join"},
            {"!games:demo.local", "@you", "join"},
            {"!science:demo.local", "@grace", "join"},
            {"!help:demo.local", "@you", "join"},
            {"!offtopic:demo.local", "@erin", "join"},
            {"!announce:demo.local", "@you", "join"},
            {"!linux:demo.local", "@dave", "join"},
            {"!linux:demo.local", "@grace", "join"},
            {"!linux:demo.local", "@you", "join"},
            {"!crypto:demo.local", "@frank", "join"},
            {"!photography:demo.local", "@carol", "join"},
            {"!travel:demo.local", "@erin", "join"},
            {"!travel:demo.local", "@you", "join"},
            {"!food:demo.local", "@bob", "join"},
            {"!books:demo.local", "@grace", "join"},
            {"!fitness:demo.local", "@you", "join"},
            {"!movies:demo.local", "@alice", "join"},
            {"!programming:demo.local", "@dave", "join"},
            {"!programming:demo.local", "@you", "join"},
            {"!rust:demo.local", "@frank", "join"},
            {"!matrix:demo.local", "@alice", "join"},
            {"!matrix:demo.local", "@you", "join"},
            {"!meta:demo.local", "@you", "join"},
               {"!sports:demo.local", "@you", "join"},
            {"!sports:demo.local", "@bob", "join"},
            {"!sports:demo.local", "@erin", "join"},
            {"!chess:demo.local", "@you", "join"},
            {"!chess:demo.local", "@charlie", "join"},
            {"!retro-gaming:demo.local", "@you", "join"},
            {"!retro-gaming:demo.local", "@carol", "join"},
            {"!hardware:demo.local", "@you", "join"},
            {"!hardware:demo.local", "@dave", "join"},
            {"!hardware:demo.local", "@alice", "join"},
            {"!distro-talk:demo.local", "@you", "join"},
            {"!distro-talk:demo.local", "@erin", "join"},
            {"!shell:demo.local", "@you", "join"},
            {"!shell:demo.local", "@frank", "join"},
            {"!editors:demo.local", "@you", "join"},
            {"!editors:demo.local", "@grace", "join"},
            {"!editors:demo.local", "@carol", "join"},
            {"!git:demo.local", "@you", "join"},
            {"!git:demo.local", "@alice", "join"},
            {"!dotfiles:demo.local", "@you", "join"},
            {"!dotfiles:demo.local", "@bob", "join"},
            {"!selfhosting:demo.local", "@you", "join"},
            {"!selfhosting:demo.local", "@charlie", "join"},
            {"!selfhosting:demo.local", "@frank", "join"},
            {"!homelab:demo.local", "@you", "join"},
            {"!homelab:demo.local", "@carol", "join"},
            {"!security:demo.local", "@you", "join"},
            {"!security:demo.local", "@dave", "join"},
            {"!privacy:demo.local", "@you", "join"},
            {"!privacy:demo.local", "@erin", "join"},
            {"!privacy:demo.local", "@bob", "join"},
            {"!networking:demo.local", "@you", "join"},
            {"!networking:demo.local", "@frank", "join"},
            {"!databases:demo.local", "@you", "join"},
            {"!databases:demo.local", "@grace", "join"},
            {"!webdev:demo.local", "@you", "join"},
            {"!webdev:demo.local", "@alice", "join"},
            {"!webdev:demo.local", "@dave", "join"},
            {"!frontend:demo.local", "@you", "join"},
            {"!frontend:demo.local", "@bob", "join"},
            {"!backend:demo.local", "@you", "join"},
            {"!backend:demo.local", "@charlie", "join"},
            {"!ml:demo.local", "@you", "join"},
            {"!ml:demo.local", "@carol", "join"},
            {"!ml:demo.local", "@grace", "join"},
            {"!ai-art:demo.local", "@you", "join"},
            {"!ai-art:demo.local", "@dave", "join"},
            {"!astronomy:demo.local", "@you", "join"},
            {"!astronomy:demo.local", "@erin", "join"},
            {"!physics:demo.local", "@you", "join"},
            {"!physics:demo.local", "@frank", "join"},
            {"!physics:demo.local", "@charlie", "join"},
            {"!chemistry:demo.local", "@you", "join"},
            {"!chemistry:demo.local", "@grace", "join"},
            {"!biology:demo.local", "@you", "join"},
            {"!biology:demo.local", "@alice", "join"},
            {"!math:demo.local", "@you", "join"},
            {"!math:demo.local", "@bob", "join"},
            {"!math:demo.local", "@erin", "join"},
            {"!history:demo.local", "@you", "join"},
            {"!history:demo.local", "@charlie", "join"},
            {"!philosophy:demo.local", "@you", "join"},
            {"!philosophy:demo.local", "@carol", "join"},
            {"!languages:demo.local", "@you", "join"},
            {"!languages:demo.local", "@dave", "join"},
            {"!languages:demo.local", "@alice", "join"},
            {"!writing:demo.local", "@you", "join"},
            {"!writing:demo.local", "@erin", "join"},
            {"!poetry:demo.local", "@you", "join"},
            {"!poetry:demo.local", "@frank", "join"},
            {"!art:demo.local", "@you", "join"},
            {"!art:demo.local", "@grace", "join"},
            {"!art:demo.local", "@carol", "join"},
            {"!pixelart:demo.local", "@you", "join"},
            {"!pixelart:demo.local", "@alice", "join"},
            {"!music-production:demo.local", "@you", "join"},
            {"!music-production:demo.local", "@bob", "join"},
            {"!synth:demo.local", "@you", "join"},
            {"!synth:demo.local", "@charlie", "join"},
            {"!synth:demo.local", "@frank", "join"},
            {"!jazz:demo.local", "@you", "join"},
            {"!jazz:demo.local", "@carol", "join"},
            {"!metal:demo.local", "@you", "join"},
            {"!metal:demo.local", "@dave", "join"},
            {"!classical:demo.local", "@you", "join"},
            {"!classical:demo.local", "@erin", "join"},
            {"!classical:demo.local", "@bob", "join"},
            {"!techno:demo.local", "@you", "join"},
            {"!techno:demo.local", "@frank", "join"},
            {"!dnb:demo.local", "@you", "join"},
            {"!dnb:demo.local", "@grace", "join"},
            {"!hiking:demo.local", "@you", "join"},
            {"!hiking:demo.local", "@alice", "join"},
            {"!hiking:demo.local", "@dave", "join"},
            {"!camping:demo.local", "@you", "join"},
            {"!camping:demo.local", "@bob", "join"},
            {"!cycling:demo.local", "@you", "join"},
            {"!cycling:demo.local", "@charlie", "join"},
            {"!running:demo.local", "@you", "join"},
            {"!running:demo.local", "@carol", "join"},
            {"!running:demo.local", "@grace", "join"},
            {"!climbing:demo.local", "@you", "join"},
            {"!climbing:demo.local", "@dave", "join"},
            {"!yoga:demo.local", "@you", "join"},
            {"!yoga:demo.local", "@erin", "join"},
            {"!vegan:demo.local", "@you", "join"},
            {"!vegan:demo.local", "@frank", "join"},
            {"!vegan:demo.local", "@charlie", "join"},
            {"!baking:demo.local", "@you", "join"},
            {"!baking:demo.local", "@grace", "join"},
            {"!coffee:demo.local", "@you", "join"},
            {"!coffee:demo.local", "@alice", "join"},
            {"!tea:demo.local", "@you", "join"},
            {"!tea:demo.local", "@bob", "join"},
            {"!tea:demo.local", "@erin", "join"},
            {"!beer:demo.local", "@you", "join"},
            {"!beer:demo.local", "@charlie", "join"},
            {"!wine:demo.local", "@you", "join"},
            {"!wine:demo.local", "@carol", "join"},
            {"!boardgames:demo.local", "@you", "join"},
            {"!boardgames:demo.local", "@dave", "join"},
            {"!boardgames:demo.local", "@alice", "join"},
            {"!podcasts:demo.local", "@you", "join"},
            {"!podcasts:demo.local", "@erin", "join"},
            {"!memes:demo.local", "@you", "join"},
            {"!memes:demo.local", "@frank", "join"},
            {"!diy:demo.local", "@you", "join"},
            {"!diy:demo.local", "@grace", "join"},
            {"!diy:demo.local", "@carol", "join"},
            {"!finance:demo.local", "@you", "join"},
            {"!finance:demo.local", "@alice", "join"},
        };
        for (auto& m : mem) {
            matrix::Event ev;
            ev.event_id = "$demo_" + std::to_string(ts);
            ev.room_id = m.room; ev.sender = m.sender;
            ev.type = "m.room.member";
            nlohmann::json content;
            content["membership"] = m.ms;
            content["displayname"] = shortName(m.sender);
            ev.content = content;
            ev.origin_server_ts = ts;
            dbi.insertEvent(ev);
            ts -= 60;
        }
    }

    // A real THREAD (m.thread relation): a root message + replies. The
    // ASCII UI and the view command render these with the thread marker.
    {
        std::string rootId = "$demo_" + std::to_string(ts);
        matrix::Event tRoot;
        tRoot.event_id = rootId;
        tRoot.room_id = "!dev:demo.local"; tRoot.sender = "@alice";
        tRoot.type = "m.room.message";
        tRoot.content = {{"body", "Let's plan the v0.5 release"},
                         {"msgtype", "m.text"}};
        tRoot.origin_server_ts = ts;
        dbi.insertEvent(tRoot);
        ts -= 60;

        struct { const char* sender; const char* body; } reps[] = {
            {"@charlie", "Agreed — how about Friday?"},
            {"@alice", "Friday works for me"},
            {"@bob", "Let me check my calendar, ETA tomorrow"},
        };
        for (auto& rp : reps) {
            matrix::Event tr;
            tr.event_id = "$demo_" + std::to_string(ts);
            tr.room_id = "!dev:demo.local"; tr.sender = rp.sender;
            tr.type = "m.room.message";
            tr.content = {{"body", rp.body}, {"msgtype", "m.text"},
                          {"m.relates_to", {{"m.thread", {{"event_id", rootId}}}}}};
            tr.origin_server_ts = ts;
            dbi.insertEvent(tr);
            ts -= 60;
        }
    }

    // Multilevel reply chain (element-web style): bob replies to alice's
    // "Welcome!", alice replies to bob's reply (reply-of-reply). The fallback
    // "> quote" block is what real clients embed in reply bodies.
    {
        std::string alice_welcome = "$demo_" + std::to_string(ts + 8 * 60);
        matrix::Event r1;
        r1.event_id = "$demo_" + std::to_string(ts);
        r1.room_id = "!general:demo.local"; r1.sender = "@bob";
        r1.type = "m.room.message";
        r1.content = {{"body",
            "> <@alice:demo.local> Welcome! This is matrixcli — a terminal Matrix client.\n\n"
            "It even renders reply chains!"},
            {"msgtype", "m.text"},
            {"m.relates_to", {{"m.in_reply_to", {{"event_id", alice_welcome}}}}}};
        r1.origin_server_ts = ts;
        dbi.insertEvent(r1);
        std::string bob_reply = r1.event_id;
        ts -= 60;

        matrix::Event r2;
        r2.event_id = "$demo_" + std::to_string(ts);
        r2.room_id = "!general:demo.local"; r2.sender = "@alice";
        r2.type = "m.room.message";
        r2.content = {{"body",
            "> <@bob:demo.local> It even renders reply chains!\n\n"
            "…and the reply to that too."},
            {"msgtype", "m.text"},
            {"m.relates_to", {{"m.in_reply_to", {{"event_id", bob_reply}}}}}};
        r2.origin_server_ts = ts;
        dbi.insertEvent(r2);
        ts -= 60;
    }

    std::cout << "Populated DB: " << (sizeof(rooms)/sizeof(rooms[0])) << " rooms, "
              << (sizeof(msgs)/sizeof(msgs[0])) + 2 + 4 << " messages (incl. a thread)." << std::endl;
    std::cout << "Try:  matrixcli rooms | matrixcli view #general | matrixcli view #dev" << std::endl;
    return 0;
}


// ---- Interactive demo REPL (offline, no Matrix account needed) ----
//
// Replaces the old `demo` behavior (which started an HTTP API server on
// port 8080). Now `matrixcli demo` drops the user into an interactive
// terminal session against the offline demo database: type commands, see
// output, same handlers as the real CLI (rooms/view/search). The web demo
// stays available as `matrixcli serve --demo`.

static void demoReplParseLine(const std::string& line, matrixcli::cli::Args& out) {
    std::istringstream iss(line);
    std::vector<std::string> words;
    std::string w;
    while (iss >> w) words.push_back(w);
    if (words.empty()) return;
    out.command = words[0];
    for (size_t i = 1; i < words.size(); ++i) {
        if (words[i].size() >= 2 && words[i][0] == '-' && words[i][1] == '-') {
            std::string key = words[i].substr(2);
            bool nextIsOpt = (i + 1 < words.size()) &&
                words[i + 1].size() >= 2 && words[i + 1][0] == '-' && words[i + 1][1] == '-';
            if (i + 1 < words.size() && !nextIsOpt) {
                out.options[key] = words[i + 1];
                ++i;
            } else {
                out.options[key] = "true";
            }
        } else {
            out.positional.push_back(words[i]);
        }
    }
}

static void demoReplSend(const matrixcli::cli::Args& args) {
    if (args.positional.size() < 2) {
        std::cout << "Usage: send <room> <message text>" << std::endl;
        return;
    }
    matrixcli::db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cout << "[demo] cannot open matrixcli.db" << std::endl;
        return;
    }
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string body;
    for (size_t i = 1; i < args.positional.size(); ++i) {
        if (!body.empty()) body += " ";
        body += args.positional[i];
    }
    matrixcli::matrix::Event ev;
    ev.event_id = "$demo_" + std::to_string(ts);
    ev.room_id = args.positional[0];
    ev.sender = "@you:demo.local";
    ev.type = "m.room.message";
    ev.content = {{"body", body}, {"msgtype", "m.text"}};
    ev.origin_server_ts = ts;
    dbi.insertEvent(ev);
    std::cout << "[demo] sent " << ts << " to " << args.positional[0] << ": " << body << std::endl;
}

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args& args);
#endif
int cmdDemoRepl(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    // Pure CLI mode: populate the demo DB and exit — the user then runs the
    // normal one-shot commands (matrixcli rooms / view / send / search).
    auto runPureCli = []() {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (dbi.listRooms().empty()) populateDemoData(dbi);
        std::cout << "Demo data ready. Use the one-shot commands:\n"
                     "  matrixcli rooms\n"
                     "  matrixcli view \"#general\" 10\n"
                     "  matrixcli send \"#general\" \"hello\"\n";
        return 0;
    };
    if (args.options.count("cli") || args.options.count("populate")) {
        return runPureCli();
    }

    // Demo + ASCII interface: 'demo --ui [room]' runs the ASCII client
    // interface; with --static/--once it draws the frame once and exits
    // (non-interactive, pipe-friendly) instead of starting the REPL.
    if (args.options.count("ui") || args.options.count("ascii")) {
        cli::Args uiArgs;
        if (!args.positional.empty()) {
            uiArgs.positional.push_back(args.positional[0]);
        }
        if (args.options.count("static") || args.options.count("once")) {
            uiArgs.options["static"] = "true";
        }
        if (args.options.count("ids")) uiArgs.options["ids"] = "true";
        if (args.options.count("time-full") || args.options.count("sec"))
            uiArgs.options["time-full"] = "true";
        if (args.options.count("right")) uiArgs.options["right"] = args.options.at("right");
        if (args.options.count("thread-root"))
            uiArgs.options["thread-root"] = args.options.at("thread-root");
        if (args.options.count("limit")) uiArgs.options["limit"] = args.options.at("limit");
        if (args.options.count("rows")) uiArgs.options["rows"] = args.options.at("rows");
        if (args.options.count("scroll")) uiArgs.options["scroll"] = args.options.at("scroll");
        return matrixcli::cmdAsciiUi(uiArgs);
    }

    // Demo + the ncurses TUI: 'demo --tui' (or choice 4) — the terminal UI
    // with the demo database populated.
    if (args.options.count("tui")) {
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (dbi.listRooms().empty()) populateDemoData(dbi);
#ifdef BUILD_TUI
        return cmdTUI(args);
#else
        std::cout << "tui not built (recompile with BUILD_TUI=ON)." << std::endl;
        return 1;
#endif
    }

    // On a terminal, let the user CHOOSE: interactive REPL, pure CLI, or
    // the ASCII-drawn client interface (rooms | chat | members).
    if (isatty(STDIN_FILENO)) {
        std::cout << "Choose demo mode:\n"
                     "  1) interactive session (type commands at a prompt)\n"
                     "  2) populate demo data and exit (one-shot commands)\n"
                     "  3) ASCII client interface (rooms | chat | members)\n"
                     "     (non-interactive: matrixcli demo --ui --static)\n"
                     "  4) terminal UI (ncurses TUI)\n"
                     "Choice [1/2/3/4]: " << std::flush;
        std::string ans;
        std::getline(std::cin, ans);
        if (!matrixcli::g_interrupted.load()) {
            // Ctrl+C at the choice prompt — leave immediately.
            std::cout << "Bye!" << std::endl;
            return 0;
        }
        if (!ans.empty() && (ans[0] == '2' || ans[0] == 'n')) {
            return runPureCli();
        }
        if (!ans.empty() && ans[0] == '3') {
            cli::Args uiArgs;
            return matrixcli::cmdAsciiUi(uiArgs);
        }
        if (!ans.empty() && ans[0] == '4') {
            cli::Args tuiArgs;
            tuiArgs.options["tui"] = "true";
            return cmdDemoRepl(tuiArgs);
        }
    }

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;
    if (dbi.listRooms().empty()) {
        std::cout << "Populating demo data..." << std::endl;
        populateDemoData(dbi);
    }

    std::cout << "matrixcli demo — interactive mode (offline, no account needed)" << std::endl;
    std::cout << "Commands: help | rooms | view <room> [n] | search <query> |"
              << " send <room> <text> | ui | clear | quit" << std::endl;
    std::cout << "Demo rooms: #general  #dev  #random  #dm_alice  #dm_bob" << std::endl;

    std::string line;
    std::vector<std::string> history;
    for (;;) {
        if (!matrixcli::g_interrupted.load()) break;  // Ctrl+C
        if (!readLineWithHistory(history, "demo> ", line)) break;
        if (!matrixcli::g_interrupted.load()) break;
        // trim
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        cli::Args a;
        demoReplParseLine(line, a);

        if (a.command == "quit" || a.command == "exit") break;
        if (a.command == "help") {
            std::cout << "  rooms                     list demo rooms\n"
                         "  view <room> [n]           show the last n messages (default 20)\n"
                         "  search <query>            full-text search in cached messages\n"
                         "  send <room> <text>        send a message (demo, offline)\n"
                         "  ui                        ASCII client interface (rooms | chat | members)\n"
                         "  clear                     clear the screen\n"
                         "  quit / exit               leave the demo\n";
            continue;
        }
        if (a.command == "clear") {
            std::cout << "\033[2J\033[H" << std::flush;
            continue;
        }
        if (a.command == "rooms") { cmdRooms(a); continue; }
        if (a.command == "view") { cmdView(a); continue; }
        if (a.command == "search") { cmdSearch(a); continue; }
        if (a.command == "send") { demoReplSend(a); continue; }
        if (a.command == "attach" || a.command == "send-file") {
            if (a.positional.size() < 2) {
                std::cout << "Usage: attach <room> <file> [--caption text]" << std::endl;
                continue;
            }
            std::string roomQ = a.positional[0];
            std::string roomId = roomQ;
            for (const auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == roomQ || name == roomQ ||
                    (!roomQ.empty() && (name.find(roomQ) == 0 ||
                                        id.find(roomQ) != std::string::npos))) {
                    roomId = id;
                    break;
                }
            }
            std::string cap = a.options.count("caption") ? a.options.at("caption") : "";
            std::string thr = a.options.count("thread") ? a.options.at("thread") : "";
            uiInsertLocalFile(dbi, roomId, a.positional[1], cap, thr);
            std::cout << "[demo] file recorded locally: " << a.positional[1]
                      << " -> " << roomId << std::endl;
            continue;
        }
        if (a.command == "ui" || a.command == "ascii") {
            int rc = matrixcli::cmdAsciiUi(a);
            std::cout << "Back in the demo session (type 'quit' to leave)." << std::endl;
            if (rc != 0) continue;
            continue;
        }
        std::cout << "Unknown command '" << a.command << "' — type 'help'." << std::endl;
    }
    std::cout << "Bye!" << std::endl;
    return 0;
}

int cmdDemoPopulate(const matrixcli::cli::Args&) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;
    return populateDemoData(dbi);
}

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    Config::instance().load("config.json");
    matrix::Client client;

    db::Database dbi;
    dbi.open("matrixcli.db");
    auto acc = dbi.loadAccount();
    if (acc.is_logged_in()) {
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);
    } else if (!Config::instance().homeserverURL().empty()) {
        client.setHomeserverURL(Config::instance().homeserverURL());
        client.setAccessToken(Config::instance().accessToken());
    }
    client.setDatabase(&dbi);

    // Initialize crypto if logged in from DB
    if (acc.is_logged_in()) {
        client.initCrypto(acc.user_id, acc.device_id);
    }

    tui::Screen screen;
    screen.init();

    // Demo TUI: skip the login view — the demo database is already
    // populated, the chat opens straight away (offline).
    tui::LoginResult login_result;
    if (args.options.count("tui")) {
        db::Database checkDb;
        if (checkDb.open("matrixcli.db") && checkDb.listRooms().empty()) {
            populateDemoData(checkDb);
        }
        login_result.success = true;
        login_result.username = "demo";
        login_result.homeserver = "demo.local";
        login_result.password = "";
    } else {
        tui::LoginView login_view;
        login_result = login_view.run(screen);
    }

    if (login_result.success) {
        try {
            client.setHomeserverURL(login_result.homeserver);
            bool demoMode = args.options.count("tui");
            db::StoredAccount sacc;
            if (demoMode) {
                // Offline demo: a fake user, no network login.
                sacc.homeserver_url = "demo.local";
                sacc.user_id = "@demo:demo.local";
                sacc.access_token = "";
                sacc.device_id = "demo-tui";
            } else {
                auto creds = client.loginPassword(login_result.username, login_result.password);
                Config::instance().set("homeserver_url", login_result.homeserver);
                Config::instance().set("access_token", creds.access_token);
                Config::instance().set("user_id", creds.user_id);
                Config::instance().set("device_id", creds.device_id);
                Config::instance().save();
                sacc.homeserver_url = login_result.homeserver;
                sacc.user_id = creds.user_id;
                sacc.access_token = creds.access_token;
                sacc.device_id = creds.device_id;
                // Init crypto
                client.initCrypto(creds.user_id, creds.device_id);
            }
            dbi.saveAccount(sacc);

            tui::ChatView chat;
            chat.setStatus("Connected as " + sacc.user_id);
            chat.setConnectionStatus(demoMode ? "demo (offline)" : "online");

            // Load TUI config
            tui::TUIConfig tuiCfg = tui::TUIConfig::load("matrixcli.toml");

            // Command handler for slash commands
            chat.setCommandHandler([&](const std::string& cmd, const std::string& args) {
                if (cmd == "me" || cmd == "emote") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendEmote(roomId, args);
                } else if (cmd == "notice") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendNotice(roomId, args);
                } else if (cmd == "join") {
                    if (!args.empty()) client.joinRoom(args);
                } else if (cmd == "leave") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.leaveRoom(roomId);
                } else if (cmd == "nick" || cmd == "name") {
                    if (!args.empty()) client.setDisplayName(args);
                } else if (cmd == "topic" || cmd == "desc") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) client.setRoomTopic(roomId, args);
                } else if (cmd == "roomname") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) client.setRoomName(roomId, args);
                } else if (cmd == "avatar") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        // If args is a file path, upload first
                        std::string url = args;
                        if (args.find("mxc://") != 0 && args.find("http") != 0) {
                            try { url = client.uploadMedia(args); } catch (...) { return; }
                        }
                        client.setRoomAvatar(roomId, url);
                    }
                } else if (cmd == "useravatar") {
                    if (!args.empty()) {
                        std::string url = args;
                        if (args.find("mxc://") != 0 && args.find("http") != 0) {
                            try { url = client.uploadMedia(args); } catch (...) { return; }
                        }
                        client.setAvatarUrl(url);

                    }
                } else if (cmd == "displayname" || cmd == "nick") {
                    if (!args.empty()) client.setDisplayName(args);
                } else if (cmd == "redact" || cmd == "delete") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) client.redactEvent(roomId, args);
                } else if (cmd == "edit") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos) {
                        nlohmann::json c = {{"msgtype","m.text"},{"body","* "+args.substr(sp+1)},
                            {"m.new_content",{{"msgtype","m.text"},{"body",args.substr(sp+1)}}},
                            {"m.relates_to",{{"event_id",args.substr(0,sp)},{"rel_type","m.replace"}}}};
                        try { client.sendEvent(roomId, "m.room.message", c); } catch (...) {}
                    }
                } else if (cmd == "knock") {
                    if (!args.empty()) try { client.knockRoom(args); } catch (...) {}
                } else if (cmd == "read" || cmd == "markread") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        try { client.sendReadReceipt(roomId, ""); } catch (...) {}
                    }
                } else if (cmd == "online") {
                    client.setPresence("online");
                } else if (cmd == "away") {
                    client.setPresence("unavailable");
                } else if (cmd == "offline") {
                    client.setPresence("offline");
                } else if (cmd == "devices") {
                } else if (cmd == "invite") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && !args.empty()) {
                        std::string user = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string reason = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        client.inviteUser(roomId, user, reason);
                    }
                } else if (cmd == "kick") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && !args.empty()) {
                        std::string user = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string reason = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        client.kickUser(roomId, user, reason);
                    }
                } else if (cmd == "ban") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && !args.empty()) {
                        std::string user = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string reason = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        client.banUser(roomId, user, reason);
                    }
                } else if (cmd == "react") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        auto sp = args.find(' ');
                        std::string eventId = (sp != std::string::npos) ? args.substr(0, sp) : "";
                        std::string key = (sp != std::string::npos) ? args.substr(sp + 1) : args;
                        if (!eventId.empty()) {
                            try { client.sendReaction(roomId, eventId, key); } catch (...) {}
                        }
                    }
                } else if (cmd == "vote") {
                    // Vote in a poll: /vote event_id answer  (or /vote event_id 1 for option number)
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        auto sp = args.find(' ');
                        std::string pollId = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string answers = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        std::vector<std::string> ansVec;
                        if (!answers.empty()) {
                            size_t pos = 0;
                            while ((pos = answers.find(',')) != std::string::npos) {
                                ansVec.push_back(answers.substr(0, pos));
                                answers.erase(0, pos + 1);
                            }
                            ansVec.push_back(answers);
                        }
                        try { client.sendPollResponse(roomId, pollId, ansVec); } catch (...) {}
                    }
                } else if (cmd == "poll") {
                    // Create poll: /poll "Question?" "Option A" "Option B" "Option C"
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        std::vector<std::string> parts;
                        bool in_quote = false;
                        std::string cur;
                        for (char c : args) {
                            if (c == '"') { in_quote = !in_quote; continue; }
                            if (c == ' ' && !in_quote && !cur.empty()) { parts.push_back(cur); cur.clear(); continue; }
                            cur += c;
                        }
                        if (!cur.empty()) parts.push_back(cur);
                        if (parts.size() >= 3) {
                            std::string question = parts[0];
                            std::vector<std::string> answers(parts.begin() + 1, parts.end());
                            try { client.sendPoll(roomId, question, answers); } catch (...) {}
                        }
                    }
                } else if (cmd == "shrug") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendTextMessage(roomId, "¯\\_(ツ)_/¯");
                } else if (cmd == "tableflip") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) client.sendTextMessage(roomId, "(╯°□°)╯︵ ┻━┻");
                } else if (cmd == "upload") {
                    std::string roomId = chat.activeRoomId();
                    if (!args.empty()) {
                        if (!roomId.empty()) {
                            try {
                                auto mxc = client.uploadMedia(args);
                                client.sendFileMessage(roomId, mxc, args, 0, "");
                    } catch (...) {}
                }
                } else if (cmd == "voice") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        try {
                            auto mxc = client.uploadMedia(args);
                            client.sendVoiceMessage(roomId, mxc, 3000);
                        } catch (...) {}
                    }
                } else if (cmd == "sticker") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        try {
                            std::string url = (args.find("mxc://") == 0) ? args : client.uploadMedia(args);
                            client.sendSticker(roomId, url, "Sticker");
                        } catch (...) {}
                    }
                } else if (cmd == "location") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        auto sp = args.find(' ');
                        std::string geo = (sp != std::string::npos) ? args.substr(0, sp) : args;
                        std::string desc = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                        try { client.sendLocation(roomId, geo, desc); } catch (...) {}
                    }
                } else if (cmd == "todo") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json items = nlohmann::json::array();
                        items.push_back({{"text", args}, {"done", false}});
                        try { client.sendTodo(roomId, "TODO", items); } catch (...) {}
                    }
                } else if (cmd == "bridge") {
                    // Bridge status — check account data for bridge info
                    chat.setConnectionStatus("Bridges: IRC/XMPP/Telegram/DeltaChat available");
                } else if (cmd == "op" || cmd == "admin") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.setPowerLevel(roomId, args, 100); } catch (...) {}
                } else if (cmd == "deop") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.setPowerLevel(roomId, args, 0); } catch (...) {}
                } else if (cmd == "whois") {
                    if (!args.empty()) {
                        try { chat.setConnectionStatus("whois " + args + ": " + client.getDisplayName(args)); } catch (...) {}
                    }
                } else if (cmd == "ignore") {
                    if (!args.empty()) try { client.ignoreUser(args); } catch (...) {}
                } else if (cmd == "unignore") {
                    if (!args.empty()) try { client.unignoreUser(args); } catch (...) {}
                } else if (cmd == "unban") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) try { client.unbanUser(roomId, args); } catch (...) {}
                } else if (cmd == "myroomnick") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json c = {{"membership","join"},{"displayname",args}};
                        try { client.sendStateEvent(roomId, "m.room.member", client.userId(), c); } catch (...) {}
                    }
                } else if (cmd == "spoiler") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json c = {{"msgtype","m.text"},{"body","||"+args+"||"},
                            {"format","org.matrix.custom.html"},
                            {"formatted_body","<span data-mx-spoiler>"+args+"</span>"}};
                        try { client.sendEvent(roomId, "m.room.message", c); } catch (...) {}
                    }
                } else if (cmd == "plain") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendTextMessage(roomId, args); } catch (...) {}
                } else if (cmd == "lenny") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { client.sendTextMessage(roomId, args + " ( ͡° ͜ʖ ͡°)"); } catch (...) {}
                } else if (cmd == "discardsession") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && client.isRoomEncrypted(roomId))
                        try { client.enableEncryption(roomId); } catch (...) {}
                } else if (cmd == "mute") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) try { client.setRoomTag(roomId, "m.lowpriority"); } catch (...) {}
                } else if (cmd == "unmute") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) try { client.deleteRoomTag(roomId, "m.lowpriority"); } catch (...) {}
                } else if (cmd == "pin") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.pinEvent(roomId, args); } catch (...) {}
                } else if (cmd == "unpin") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.unpinEvent(roomId, args); } catch (...) {}
                } else if (cmd == "pins") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        try {
                            auto p = client.getPinnedEvents(roomId);
                            int cnt = p.value("pinned", nlohmann::json::array()).size();
                            chat.setConnectionStatus("📌 " + std::to_string(cnt) + " pinned messages");
                        } catch (...) {}
                    }
                } else if (cmd == "roomsbrowse" || cmd == "explore") {
                    if (!args.empty()) {
                        try {
                            auto pubs = client.getPublicRooms("", args, 20);
                            int total = pubs.value("total_room_count_estimate", 0);
                            chat.setConnectionStatus("Browse: " + std::to_string(total) + " rooms matching '" + args + "'");
                        } catch (...) {}
                    }
                } else if (cmd == "preview") {
                    // Show link preview via Matrix preview_url API
                    if (!args.empty()) {
                        try {
                            auto p = client.getURLPreview(args);
                            if (p.contains("og:title"))
                                chat.setConnectionStatus(p["og:title"].get<std::string>() +
                                    (p.contains("og:description") ? " — " + p["og:description"].get<std::string>() : ""));
                        } catch (...) {}
                    }
                } else if (cmd == "stats") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        try {
                            auto st = client.getRoomStats(roomId);
                            chat.setConnectionStatus("Stats: " + std::to_string(st.value("total_messages", 0)) +
                                " msgs, " + std::to_string(st.value("unique_posters", 0)) + " posters");
                        } catch (...) {}
                    }
                } else if (cmd == "fav" || cmd == "favorite") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) try { client.setRoomTag(roomId, "m.favourite"); } catch (...) {}
                } else if (cmd == "mirror") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos)
                        try { client.mirrorMessage(roomId, args.substr(0, sp), args.substr(sp + 1)); } catch (...) {}
                } else if (cmd == "markdown") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        nlohmann::json content = {{"msgtype", "m.text"}, {"body", args},
                            {"format", "org.matrix.custom.html"}, {"formatted_body", "<p>" + args + "</p>"}};
                        try { client.sendEvent(roomId, "m.room.message", content); } catch (...) {}
                    }
                } else if (cmd == "upgrade" || cmd == "upgraderoom") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { chat.setConnectionStatus("Upgraded " + client.upgradeRoom(roomId)); } catch (...) {}
                } else if (cmd == "export") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        try {
                            std::string fmt = args.empty() ? "text" : args;
                            std::string out = client.exportRoom(roomId, fmt);
                            std::ofstream ofs("export_" + roomId + "." + (fmt == "json" ? "json" : fmt == "html" ? "html" : "txt"));
                            ofs << out;
                            chat.setConnectionStatus("Exported to " + std::string(fmt == "json" ? "json" : fmt == "html" ? "html" : "txt"));
                        } catch (...) { chat.setConnectionStatus("Export failed"); }
                    }
                } else if (cmd == "statusmsg") {
                    if (!args.empty()) {
                        auto sp = args.find(' ');
                        std::string emoji = (sp != std::string::npos) ? args.substr(0, sp) : "";
                        std::string text = (sp != std::string::npos) ? args.substr(sp + 1) : args;
                        try { client.setCustomStatus(text, emoji); chat.setConnectionStatus("Status set"); } catch (...) {}
                    }
                } else if (cmd == "remind" || cmd == "reminder") {
                    auto sp = args.find(' ');
                    if (sp != std::string::npos) {
                        int secs = std::stoi(args.substr(sp + 1));
                        chat.setConnectionStatus("Reminder set in " + std::to_string(secs) + "s");
                    }
                } else if (cmd == "notify") {
                    auto sp = args.find(' ');
                    std::string sub = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    std::string val = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                    if (sub == "add" && !val.empty()) {
                        g_notifyKeywords.push_back(val);
                        chat.setConnectionStatus("Notify keyword added: " + val);
                    } else if (sub == "remove" && !val.empty()) {
                        auto it = std::find(g_notifyKeywords.begin(), g_notifyKeywords.end(), val);
                        if (it != g_notifyKeywords.end()) g_notifyKeywords.erase(it);
                        chat.setConnectionStatus("Notify keyword removed: " + val);
                    } else if (sub == "list") {
                        std::string list;
                        for (auto& k : g_notifyKeywords) list += k + " ";
                        chat.setConnectionStatus("Keywords: " + (list.empty() ? "(none)" : list));
                    }
                } else if (cmd == "directory" || cmd == "dir") {
                    if (!args.empty()) {
                        try {
                            auto pubs = client.getPublicRooms("", args, 20);
                            int cnt = pubs.value("total_room_count_estimate", 0);
                            chat.setConnectionStatus("Directory: " + std::to_string(cnt) + " rooms matching '" + args + "'");
                        } catch (...) {}
                    }
                } else if (cmd == "spell") {
                    // Simple spell check — find closest command
                    if (!args.empty()) {
                        std::vector<std::string> cmds = {"join","leave","kick","ban","invite","op","deop",
                            "whois","ignore","pin","unpin","pins","stats","fav","mirror","markdown","upgrade",
                            "export","statusmsg","remind","notify","directory","nick","topic","react","vote",
                            "search","voice","sticker","location","todo","create","upload","redact","read","online","away"};
                        std::string best;
                        int bestDist = 999;
                        for (auto& c : cmds) {
                            int dist = 0;
                            for (size_t i = 0; i < std::min(args.size(), c.size()); i++)
                                if (tolower(args[i]) != tolower(c[i])) dist++;
                            dist += std::abs((int)args.size() - (int)c.size());
                            if (dist < bestDist) { bestDist = dist; best = c; }
                        }
                        chat.setConnectionStatus("Did you mean: /" + best + " ?");
                    }
                } else if (cmd == "rainbow") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendTextMessage(roomId, args); } catch (...) {}
                } else if (cmd == "rainbowme") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendEmote(roomId, args); } catch (...) {}
                } else if (cmd == "confetti") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { client.sendTextMessage(roomId, args + " 🎉✨🎊"); } catch (...) {}
                } else if (cmd == "snowfall") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty())
                        try { client.sendTextMessage(roomId, args + " ❄️🌨️❄️"); } catch (...) {}
                } else if (cmd == "myroomavatar") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty()) {
                        std::string url = (args.find("mxc://") == 0) ? args : client.uploadMedia(args);
                        nlohmann::json c = {{"membership","join"},{"avatar_url",url}};
                        try { client.sendStateEvent(roomId, "m.room.member", client.userId(), c); } catch (...) {}
                    }
                } else if (cmd == "report") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos)
                        try { client.sendEvent(roomId, "m.room.report",
                            nlohmann::json{{"event_id",args.substr(0,sp)},{"reason",args.substr(sp+1)}}); } catch (...) {}
                } else if (cmd == "forward") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos) {
                        auto msgs = client.getRoomMessages(roomId, args.substr(0, sp));
                        if (!msgs.empty())
                            try { client.sendTextMessage(args.substr(sp+1),
                                "[Fwd from " + roomId + "] <" + msgs[0].sender + "> " + msgs[0].content.value("body","")); } catch (...) {}
                    }
                } else if (cmd == "schedule") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos)
                        chat.setConnectionStatus("Scheduled: " + args.substr(0, sp) + "s → " + args.substr(sp+1));
                } else if (cmd == "users") {
                    if (!args.empty())
                        try { auto r = client.searchUserDirectory(args); chat.setConnectionStatus(
                            std::to_string(r.value("results",nlohmann::json::array()).size()) + " users for '" + args + "'"); } catch (...) {}
                } else if (cmd == "createspace") {
                    if (!args.empty())
                        try { client.createRoom(args, "", false, {}); chat.setConnectionStatus("Space created"); } catch (...) {}
                } else if (cmd == "addtospace") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty() && !args.empty())
                        try { client.sendStateEvent(args, "m.space.child", roomId,
                            nlohmann::json{{"via",nlohmann::json::array({""})},{"suggested",false},{"auto_join",false}}); } catch (...) {}
                } else if (cmd == "joinspace") {
                    if (!args.empty()) try { client.joinRoom(args); } catch (...) {}
                } else if (cmd == "admin") {
                    auto sp = args.find(' ');
                    std::string sub = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    std::string val = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                    if (sub == "deactivate" && !val.empty())
                        try { client.adminDeactivateUser(val); chat.setConnectionStatus("Deactivated " + val); } catch (...) {}
                    else if (sub == "resetpw" && !val.empty()) {
                        auto sp2 = val.find(' '); auto uid = sp2 != std::string::npos ? val.substr(0, sp2) : val;
                        auto pw = sp2 != std::string::npos ? val.substr(sp2 + 1) : "";
                        try { client.adminResetPassword(uid, pw); chat.setConnectionStatus("Password reset for " + uid); } catch (...) {}
                    } else if (sub == "listusers")
                        try { auto u = client.adminListUsers(); chat.setConnectionStatus("Users: " + std::to_string(u.value("total", 0))); } catch (...) {}
                    else if (sub == "deleteroom" && !val.empty())
                        try { client.adminDeleteRoom(val); chat.setConnectionStatus("Room deleted"); } catch (...) {}
                    else if (sub == "shadowban" && !val.empty())
                        try { client.adminShadowBan(val); chat.setConnectionStatus("Shadow banned " + val); } catch (...) {}
                    else if (sub == "roomstats")
                        try { client.adminRoomStats(); chat.setConnectionStatus("Room stats fetched"); } catch (...) {}
                } else if (cmd == "td") {
                    auto sp = args.find(' ');
                    std::string sub = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    std::string val = (sp != std::string::npos) ? args.substr(sp + 1) : "";
                    if (sub == "login" || sub == "start") {
                        if (!g_tdlib.isAvailable()) { g_tdlib.initialize(); }
                        if (g_tdlib.isAvailable()) {
                            // Use test API credentials (you need real ones for production)
                            g_tdlib.setTdlibParams(94575, "a3406de8d171bb422bb6ddf3bbd8f4e2");
                            chat.setConnectionStatus("TDLib initialized. Send /td phone +123****7890");
                        } else chat.setConnectionStatus("TDLib not available (install libtdjson)");
                    } else if (sub == "phone") {
                        if (!val.empty()) { g_tdlib.sendPhoneNumber(val); chat.setConnectionStatus("Sent code to " + val + ". /td code XXXXX"); }
                    } else if (sub == "code") {
                        if (!val.empty()) { g_tdlib.sendAuthCode(val); chat.setConnectionStatus("Code sent. /td password your2fa (if needed)"); }
                    } else if (sub == "password" || sub == "2fa") {
                        if (!val.empty()) { g_tdlib.sendPassword(val); chat.setConnectionStatus("2FA sent"); }
                    } else if (sub == "chats") {
                        if (g_tdlib.authState() == matrixcli::tdlib::TdAuthState::Ready) {
                            auto chats = g_tdlib.getChats(20);
                            std::string list = std::to_string(chats.size()) + " chats: ";
                            for (size_t i = 0; i < std::min((size_t)5, chats.size()); i++)
                                list += chats[i].title + (i < 4 ? ", " : "");
                            chat.setConnectionStatus(list);
                        } else chat.setConnectionStatus("Not authorized. /td phone first");
                    } else if (sub == "msg") {
                        if (g_tdlib.authState() == matrixcli::tdlib::TdAuthState::Ready) {
                            auto sp2 = val.find(' ');
                            if (sp2 != std::string::npos) {
                                int64_t chatId = std::stoll(val.substr(0, sp2));
                                g_tdlib.sendMessage(chatId, val.substr(sp2 + 1));
                                chat.setConnectionStatus("Sent to Telegram");
                            }
                        }
                    } else if (sub == "history") {
                        if (g_tdlib.authState() == matrixcli::tdlib::TdAuthState::Ready && !val.empty()) {
                            int64_t chatId = std::stoll(val);
                            auto msgs = g_tdlib.getChatHistory(chatId);
                            std::string preview = std::to_string(msgs.size()) + " msgs. Latest: ";
                            if (!msgs.empty()) preview += msgs[0].text.substr(0, 60);
                            chat.setConnectionStatus(preview);
                        }
                    } else {
                        chat.setConnectionStatus("TDLib: /td login|phone|code|password|chats|msg|history");
                    }
                }
                } else if (cmd == "create" || cmd == "newroom") {
                    auto sp = args.find(' ');
                    std::string name = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    try {
                        auto roomId = client.createRoom(name);
                        client.joinRoom(roomId);
                    } catch (...) {}
                } else if (cmd == "search" || cmd == "find") {
                    // Full-text search messages
                    if (!args.empty()) {
                        try {
                            auto results = dbi.search(args, 20);
                            if (!results.empty()) {
                                std::string output;
                                for (auto& r : results) {
                                    std::string body = r.value("content", nlohmann::json::object()).value("body", "");
                                    output += body.substr(0, 80) + " | ";
                                }
                                chat.setConnectionStatus("Search: " + std::to_string(results.size()) + " results");
                            } else {
                                chat.setConnectionStatus("Search: no results");
                            }
                        } catch (...) {}
                    }
                } else if (cmd == "joinroom") {
                    // Join room by name or alias
                    if (!args.empty()) {
                        try { client.joinRoom(args); } catch (...) {}
                    }
                } else if (cmd == "preview" && !args.empty()) {
                    try {
                        auto preview = client.getURLPreview(args);
                        if (preview.contains("og:title")) {
                            // Show URL preview in status
                            chat.setConnectionStatus("Preview: " + preview["og:title"].get<std::string>());
                        }
                    } catch (...) {}
                }
            });

            // Load rooms from DB
            auto rooms = dbi.listRooms();
            std::vector<tui::RoomInfo> roomInfos;
            for (auto& r : rooms) {
                tui::RoomInfo ri;
                ri.id = r.value("room_id", "");
                ri.name = r.value("name", "");
                if (ri.name.empty()) ri.name = ri.id;
                ri.is_encrypted = r.value("is_encrypted", false);
                roomInfos.push_back(ri);
            }
            if (roomInfos.empty()) {
                // Add a placeholder
                tui::RoomInfo ri;
                ri.id = "!welcome:demo.local";
                ri.name = "#welcome";
                roomInfos.push_back(ri);
            }
            chat.setRooms(roomInfos);

            // Set up send callback with retry queue
            chat.setSendCallback([&](const std::string& body) {
                std::string roomId = chat.activeRoomId();
                if (!roomId.empty()) {
                    try {
                        client.sendTextMessage(roomId, body);
                        client.sendTyping(roomId, false);
                    } catch (...) {
                        // Queue for retry
                        std::lock_guard<std::mutex> lock(g_queueMutex);
                        g_msgQueue[roomId].push_back({body, 0});
                        chat.setConnectionStatus("Queued (will retry): " + body.substr(0, 40));
                    }
                }
            });

            // Set up pagination callback
            chat.setPaginateCallback([&](const std::string& room_id) {
                try {
                    client.getRoomMessages(room_id, "", "b", 50);
                } catch (...) {}
            });

            // Start sync: feed events to chat, flush message queue
            client.startSync([&](const matrix::Event& ev) {
                // Flush queued messages on successful sync
                {
                    std::lock_guard<std::mutex> lock(g_queueMutex);
                    for (auto& [rid, msgs] : g_msgQueue) {
                        for (auto it = msgs.begin(); it != msgs.end();) {
                            try {
                                client.sendTextMessage(rid, it->first);
                                it = msgs.erase(it);
                            } catch (...) {
                                it->second++;
                                if (it->second > 5) it = msgs.erase(it); // give up after 5 retries
                                else ++it;
                            }
                        }
                    }
                }
                tui::RoomInfo ri;
                ri.id = ev.room_id;
                ri.name = ev.room_id;
                chat.addRoom(ri);

                // Evaluate push rules for notification
                nlohmann::json jev;
                jev["event_id"] = ev.event_id;
                jev["room_id"] = ev.room_id;
                jev["sender"] = ev.sender;
                jev["type"] = ev.type;
                jev["content"] = ev.content;
                auto pr = client.evaluatePush(jev);

                if (ev.type == "m.room.message" && ev.content.contains("body")) {
                    tui::MessageInfo mi;
                    mi.sender = ev.sender;
                    mi.body = ev.content["body"].get<std::string>();
                    mi.event_id = ev.event_id;
                    std::string mt = ev.content.value("msgtype", "m.text");
                    mi.is_notice = (mt == "m.notice");
                    mi.is_emote = (mt == "m.emote");
                    mi.is_highlight = pr.highlight;
                    mi.url = ev.content.value("url", "");
                    mi.mimetype = ev.content.value("info", nlohmann::json::object()).value("mimetype", "");

                    // Thread support
                    if (ev.content.contains("m.relates_to")) {
                        auto& rel = ev.content["m.relates_to"];
                        std::string relType = rel.value("rel_type", "");
                        if (relType == "m.thread") {
                            mi.thread_id = rel.value("event_id", "");
                            // Mark thread root
                            bool is_root = rel.value("is_falling_back", true);
                            if (!is_root) mi.is_thread_root = false;
                        } else if (relType == "m.replace") {
                            mi.is_edited = true;
                            mi.body = ev.content.value("m.new_content", nlohmann::json::object()).value("body", mi.body);
                        }
                    }

                    chat.addMessage(ev.room_id, mi);
                }

                // Redactions
                if (ev.type == "m.room.redaction" && !ev.redacts.empty()) {
                    tui::MessageInfo mi;
                    mi.sender = ev.sender;
                    mi.body = "Message redacted";

                    mi.event_id = ev.redacts;
                    mi.is_redacted = true;
                    mi.redacted_by = ev.sender;
                    chat.addMessage(ev.room_id, mi);
                }

                // Polls
                if (ev.type == "m.poll.start" && ev.content.contains("m.poll")) {
                    tui::MessageInfo mi;
                    mi.sender = ev.sender;
                    auto& poll = ev.content["m.poll"];
                    mi.body = poll.value("question", nlohmann::json::object())
                                   .value("body", "(poll)");
                    mi.is_poll = true;
                    mi.event_id = ev.event_id;
                    for (auto& ans : poll.value("answers", nlohmann::json::array())) {
                        std::string text = ans.value("body", nlohmann::json::object()).value("body", "?");
                        mi.poll_options.emplace_back(text, 0);
                    }
                    chat.addMessage(ev.room_id, mi);
                }
                if (ev.type == "m.poll.response" && ev.content.contains("m.poll.response")) {
                    // Update vote counts by re-reading room state
                }

                // Typing events with monitor
                if (ev.type == "m.typing" && ev.content.contains("user_ids")) {
                    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    for (auto& uid : ev.content["user_ids"])
                        g_typing.updateUser(ev.room_id, uid.get<std::string>(), now);
                    g_typing.pruneExpired(now);
                    auto typing = g_typing.formatTypingUsers(ev.room_id);
                    if (!typing.empty()) chat.setConnectionStatus(typing);
                    std::vector<std::string> users;
                    for (auto& uid : ev.content["user_ids"]) users.push_back(uid.get<std::string>());
                    chat.setTypingUsers(ev.room_id, users);
                }

                // Server notices
                if (ev.type == "m.server_notice") {
                    std::string body = ev.content.value("body", "");
                    if (!body.empty()) chat.setConnectionStatus("[SERVER] " + body);
                }

                chat.requestRedraw();
            });

            chat.run(screen);
            client.stopSync();
        } catch (const std::exception& e) {
            screen.shutdown();
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
    }

    screen.shutdown();
    return 0;
}
#endif

int main(int argc, char* argv[]) {
    // sigaction WITHOUT SA_RESTART: the blocking stdin read must return
    // EINTR on Ctrl+C so the REPL loops can observe g_interrupted. The
    // plain signal() (glibc) restarts the read and Ctrl+C looks dead.
    {
        struct sigaction sa{};
        sa.sa_handler = signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
    }
    signal(SIGTERM, signalHandler);
    bool quietStart = argc >= 2 && std::string(argv[1]) == "about";
    progressive::crash::installCrashHandler(quietStart);

    // Register all commands via registry (extensible, no if/else)
    extern void registerBuiltinCommands();
    registerBuiltinCommands();

    auto args = matrixcli::cli::parseArgs(argc, argv);

    // Apply the persisted proxy (config.json "proxy_*") to the core's global
    // proxy BEFORE any command runs — every request then goes through it.
    extern void applyProxyFromConfig();
    applyProxyFromConfig();

    if (args.options.contains("version")) {
        matrixcli::cli::printVersion();
        return 0;
    }

    if (args.command.empty() || args.command == "help" || args.options.contains("help")) {
        matrixcli::cli::printUsage();
        return 0;
    }

    if (args.command == "version") {
        matrixcli::cli::printVersion();
        return 0;
    }

    if (args.command == "serve") {
        return cmdServe(args);
    }

    if (args.command == "login") {
        return cmdLogin(args);
    }

    if (args.command == "rooms") {
        return cmdRooms(args);
    }

    if (args.command == "view") {
        return cmdView(args);
    }

    if (args.command == "status") {
        return cmdStatus(args);
    }

    if (args.command == "send") {
        return cmdSendMsg(args);
    }

    if (args.command == "attach" || args.command == "send-file") {
        return matrixcli::cmdAttachFile(args);
    }

    if (args.command == "search") {
        return cmdSearch(args);
    }

    if (args.command == "config") {
        return cmdConfig(args);
    }

    if (args.command == "demo") {
        if (!args.positional.empty() && args.positional[0] == "populate") {
            return cmdDemoPopulate(args);
        }
        return cmdDemoRepl(args);
    }

    if (args.command == "ui" || args.command == "ascii") {
        return matrixcli::cmdAsciiUi(args);
    }

#ifdef BUILD_TUI
    if (args.command == "tui") {
        return cmdTUI(args);
    }
#endif

    // Default: launch TUI if available
#ifdef BUILD_TUI
    if (args.command.empty()) {
        return cmdTUI(args);
    }
#endif

    if (args.command == "td") {
        // matrixcli td <subcommand> [args...]
        using namespace matrixcli;
        if (args.positional.empty()) {
            std::cerr << "Usage: matrixcli td <login|phone|code|password|chats|msg|history>" << std::endl;
            return 1;
        }
        std::string sub = args.positional[0];

        if (sub == "login" || sub == "start") {
            if (!g_tdlib.isAvailable()) g_tdlib.initialize();
            if (!g_tdlib.isAvailable()) { std::cerr << "TDLib not available" << std::endl; return 1; }
            g_tdlib.setTdlibParams(94575, "a3406de8d171bb422bb6ddf3bbd8f4e2");
            std::cout << "TDLib initialized. Run: matrixcli td phone +123****7890" << std::endl;
        } else if (sub == "phone") {
            if (args.positional.size() < 2) { std::cerr << "Usage: matrixcli td phone +123****7890" << std::endl; return 1; }
            if (!g_tdlib.isAvailable() && !g_tdlib.initialize()) { std::cerr << "TDLib not available" << std::endl; return 1; }
            g_tdlib.sendPhoneNumber(args.positional[1]);
            std::cout << "Code sent. Run: matrixcli td code XXXXX" << std::endl;
        } else if (sub == "code") {
            if (args.positional.size() < 2) { std::cerr << "Usage: matrixcli td code XXXXX" << std::endl; return 1; }
            g_tdlib.sendAuthCode(args.positional[1]);
            std::cout << "Code sent. If 2FA: matrixcli td password yourpassword" << std::endl;
        } else if (sub == "password" || sub == "2fa") {
            if (args.positional.size() < 2) { std::cerr << "Usage: matrixcli td password your2fa" << std::endl; return 1; }
            g_tdlib.sendPassword(args.positional[1]);
            std::cout << "2FA sent. Run: matrixcli td chats" << std::endl;
        } else if (sub == "chats") {
            if (g_tdlib.authState() != tdlib::TdAuthState::Ready) { std::cerr << "Not authorized" << std::endl; return 1; }
            auto chats = g_tdlib.getChats(50);
            for (auto& c : chats) {
                std::cout << "  [" << c.id << "] " << c.title << " (" << c.type << ")" << " unread:" << c.unread_count << std::endl;
            }
        } else if (sub == "msg" || sub == "send") {
            if (args.positional.size() < 3) { std::cerr << "Usage: matrixcli td msg <chat_id> <text>" << std::endl; return 1; }
            int64_t chatId = std::stoll(args.positional[1]);
            std::string text;
            for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) text += " "; text += args.positional[i]; }
            if (g_tdlib.authState() != tdlib::TdAuthState::Ready) { std::cerr << "Not authorized" << std::endl; return 1; }
            g_tdlib.sendMessage(chatId, text);
            std::cout << "Sent to chat " << chatId << std::endl;
        } else if (sub == "history" || sub == "view") {
            if (args.positional.size() < 2) { std::cerr << "Usage: matrixcli td history <chat_id> [limit]" << std::endl; return 1; }
            int64_t chatId = std::stoll(args.positional[1]);
            int limit = args.positional.size() >= 3 ? std::stoi(args.positional[2]) : 20;
            if (g_tdlib.authState() != tdlib::TdAuthState::Ready) { std::cerr << "Not authorized" << std::endl; return 1; }
            auto msgs = g_tdlib.getChatHistory(chatId, 0, limit);
            for (auto& m : msgs) {
                std::cout << (m.is_outgoing ? "  → " : "  ← ") << m.text.substr(0, 100) << std::endl;
            }
        } else if (sub == "status") {
            static const char* states[] = {"Closed","WaitParams","WaitPhone","WaitCode","WaitPassword","Ready","LoggingOut","Error"};
            int s = (int)g_tdlib.authState();
            std::cout << "TDLib: " << (g_tdlib.isAvailable() ? "available" : "not available")
                      << ", auth: " << (s >= 0 && s < 8 ? states[s] : "unknown") << std::endl;
        } else {
            std::cerr << "Unknown td subcommand: " << sub << std::endl;
            return 1;
        }
        return 0;
    }

    if (args.command == "irc") {
        using namespace matrixcli;
        if (args.positional.empty()) {
            std::cerr << "Usage: matrixcli irc <connect|join|msg|leave|whois|names>" << std::endl;
            return 1;
        }
        static irc::IrcClient ircClient;
        static bool ircSetup = false;
        std::string sub = args.positional[0];

        if (sub == "connect") {
            irc::IrcServerConfig cfg;
            cfg.host = args.positional.size() > 1 ? args.positional[1] : "irc.libera.chat";
            cfg.port = args.positional.size() > 2 ? std::stoi(args.positional[2]) : 6667;
            cfg.nick = args.positional.size() > 3 ? args.positional[3] : "matrixcli";
            ircClient.setConfig(cfg);
            if (!ircSetup) {
                ircClient.onMessage([](const irc::IrcMessage& msg) {
                    std::cout << "  [" << msg.target << "] <" << msg.prefix << "> " << msg.body << std::endl;
                });
                ircClient.onStateChange([](irc::IrcState s) {
                    const char* names[] = {"Disconnected","Connecting","Connected","Registered","Error"};
                    std::cout << "IRC: " << names[(int)s] << std::endl;
                });
                ircSetup = true;
            }
            ircClient.connect();
            std::this_thread::sleep_for(std::chrono::seconds(3));
        } else if (sub == "join" && args.positional.size() >= 2) {
            ircClient.join(args.positional[1]);
        } else if (sub == "msg" && args.positional.size() >= 3) {
            std::string text;
            for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) text += " "; text += args.positional[i]; }
            ircClient.privmsg(args.positional[1], text);
        } else if (sub == "leave" && args.positional.size() >= 2) {
            ircClient.part(args.positional[1]);
        } else if (sub == "whois" && args.positional.size() >= 2) {
            ircClient.whois(args.positional[1]);
        } else if (sub == "names" && args.positional.size() >= 2) {
            ircClient.names(args.positional[1]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return 0;
    }

    if (args.command == "lemmy") {
        using namespace matrixcli;
        if (args.positional.empty()) {
            std::cerr << "Usage: matrixcli lemmy <login|posts|post|upvote|comments>" << std::endl;
            return 1;
        }
        std::string sub = args.positional[0];
        if (sub == "login" && args.positional.size() >= 4) {
            g_lemmy.setInstance(args.positional[1]);
            if (g_lemmy.login(args.positional[2], args.positional[3]))
                std::cout << "Logged in to " << args.positional[1] << std::endl;
            else { std::cerr << "Login failed" << std::endl; return 1; }
        } else if ((sub == "posts" || sub == "hot")) {
            std::string comm = args.positional.size() > 1 ? args.positional[1] : "";
            auto posts = g_lemmy.listPosts(comm, "Hot", 20);
            for (auto& p : posts)
                std::cout << "  [" << p.id << "] " << p.title << " (↑" << p.upvotes << " ↓" << p.downvotes << " 💬" << p.comment_count << ") " << p.community_name << std::endl;
        } else if (sub == "comments" && args.positional.size() >= 2) {
            auto comments = g_lemmy.listComments(std::stoi(args.positional[1]));
            for (auto& c : comments) std::cout << "  " << c.creator_name << ": " << c.content.substr(0, 100) << " (↑" << c.score << ")" << std::endl;
        } else if (sub == "post" && args.positional.size() >= 4) {
            std::string body;
            for (size_t i = 3; i < args.positional.size(); i++) { if (i > 3) body += " "; body += args.positional[i]; }
            int id = g_lemmy.createPost(args.positional[1], args.positional[2], body);
            std::cout << "Posted [" << id << "]" << std::endl;
        } else if (sub == "upvote" && args.positional.size() >= 2) {
            g_lemmy.likePost(std::stoi(args.positional[1]), 1); std::cout << "Upvoted" << std::endl;
        } else if (sub == "downvote" && args.positional.size() >= 2) {
            g_lemmy.likePost(std::stoi(args.positional[1]), -1); std::cout << "Downvoted" << std::endl;
        }
        return 0;
    }

    if (args.command == "dc" || args.command == "deltachat") {
        using namespace matrixcli;
        if (args.positional.empty()) { std::cerr << "Usage: matrixcli dc <login|chats|msg|history>" << std::endl; return 1; }
        std::string sub = args.positional[0];
        if (sub == "login") {
            g_dc.initialize();
            if (!g_dc.isAvailable()) { std::cerr << "DeltaChat not available (install libdeltachat)" << std::endl; return 1; }
            // Configure email
            if (args.positional.size() >= 3) {
                g_dc.setConfig("addr", args.positional[1]);
                g_dc.setConfig("mail_pw", args.positional[2]);
            }
            if (g_dc.configure()) std::cout << "Configured!" << std::endl;
            else { std::cerr << "Configure failed" << std::endl; return 1; }
        } else if (sub == "chats") {
            if (!g_dc.isConfigured()) { std::cerr << "Not configured" << std::endl; return 1; }
            auto chats = g_dc.getChatList();
            for (auto& c : chats)
                std::cout << "  [" << c.id << "] " << c.name << " (" << c.type << ")" << (c.is_verified ? " ✓" : "") << std::endl;
        } else if (sub == "msg" && args.positional.size() >= 3) {
            std::string text;
            for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) text += " "; text += args.positional[i]; }
            int msgId = g_dc.sendMessage(std::stoi(args.positional[1]), text);
            std::cout << "Sent [" << msgId << "]" << std::endl;
        } else if (sub == "history" && args.positional.size() >= 2) {
            auto msgs = g_dc.getChatMessages(std::stoi(args.positional[1]));
            for (auto& m : msgs)
                std::cout << (m.is_outgoing ? "  → " : "  ← ") << m.sender_name << ": " << m.text.substr(0, 100) << std::endl;
        }
        return 0;
    }

    // Try command registry (extensible, no if/else needed)
    auto cliHandler = matrixcli::CommandRegistry::instance().findCli(args.command);
    if (cliHandler) return cliHandler(args);

    std::cerr << "Unknown command: " << args.command << "\n"
              << "Run 'matrixcli --help' for usage." << std::endl;
    return 1;
}

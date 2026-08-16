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
#include "cli/args.hpp"
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
#include "../lib/tui/agent_setup.hpp"
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
    std::atomic<bool> g_agentInterrupt{false};
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

// The SAS verification core (defined in e2ee_commands.cpp) — shared by the
// CLI `verify` command and the TUI /verify slash.
extern int runSasVerification(const std::string& targetUser,
                              const std::string& targetDevice,
                              int timeoutSec, bool autoConfirm,
                              const std::function<void(const std::string&)>& log,
                              const std::function<bool()>& confirm);

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
        std::cout << "  Status:     " << (synced ? ANSI_GREEN "● synced" ANSI_RESET : "○ not synced — run 'progressive-cli serve'") << std::endl;
        std::cout << ANSI_CYAN "\n  ── Cache ──\n" ANSI_RESET;
        std::cout << "  Rooms:      " << rooms_c << std::endl;
        std::cout << "  Messages:   " << msgs_c << std::endl;
        std::cout << "  Unread:     " << notif_c << std::endl;
        std::cout << "\n  • Run " ANSI_BOLD "progressive-cli serve" ANSI_RESET " to sync messages into the cache\n";
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
        std::cout << "\n  Not logged in. Use 'progressive-cli login' to authenticate.\n";
        std::cout << "  Or try offline demo: progressive-cli demo populate\n";
    }

    // Smart suggestions
    {
        std::cout << ANSI_CYAN "\n  ── Suggestions ──\n" ANSI_RESET;
        bool synced = !acc.next_batch.empty();
        int notif = dbi.getNotificationCount();
        if (!synced && acc.is_logged_in())
            std::cout << "  • Run " ANSI_BOLD "progressive-cli serve" ANSI_RESET " to start syncing\n";
        if (notif > 0)
            std::cout << "  • " << notif << " unread — " ANSI_BOLD "progressive-cli view room" ANSI_RESET " to read\n";
        if (g_tdlib.isAvailable() && g_tdlib.authState() != tdlib::TdAuthState::Ready)
            std::cout << "  • TDLib ready — " ANSI_BOLD "matrixcli td login" ANSI_RESET " for Telegram\n";
        if (!g_lemmy.isLoggedIn())
            std::cout << "  • Lemmy available — " ANSI_BOLD "matrixcli lemmy login" ANSI_RESET "\n";
        std::cout << "  • All commands: " ANSI_BOLD "progressive-cli --help" ANSI_RESET "\n";
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
        std::cout << "  For demo:   progressive-cli demo    (start demo server, then try again)" << std::endl;
        std::cout << "  For real:   progressive-cli login   (login to Matrix first)" << std::endl;
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
        std::cerr << "Usage: progressive-cli view <room> [limit] [--thread event_id] [--before eid] [--from eid]\n"
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
        std::cerr << "Usage: progressive-cli attach <room> <file> [--caption text] [--thread event_id]" << std::endl;
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
        std::cerr << "Not logged in. Run 'progressive-cli login' first." << std::endl;
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
        std::cerr << "Usage: progressive-cli send <room> [--thread event_id] <message>" << std::endl;
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
        std::cerr << "Not logged in. Run 'progressive-cli login' first." << std::endl;
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
        std::cerr << "Usage: progressive-cli search <query> [--limit N|all]" << std::endl;
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
        std::cout << "(Indexed during sync. Start: progressive-cli serve, then sync populates FTS)" << std::endl;
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

// A proper-looking display name for the demo members (Alice, Bob, ...) —
// it differs from the mxid localpart, so the user list shows the
// highlighted "Displayname (mxid)" form.
// Populate the offline demo database (rooms + messages + reply chain).
// Shared by `demo populate` and the interactive `demo` REPL.
// populateDemoData, cmdDemoRepl and cmdTUI moved to demo_tui.cpp.
extern int populateDemoData(matrixcli::db::Database& dbi);
extern int cmdDemoRepl(const matrixcli::cli::Args& args);
#ifdef BUILD_TUI
extern int cmdTUI(const matrixcli::cli::Args& args);
#endif
int cmdDemoPopulate(const matrixcli::cli::Args&) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;
    return populateDemoData(dbi);
}

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

    // Just "--mobile" (no command): launch the smartphone ASCII UI right
    // away — Element Classic style, Termux-friendly. Any extra flags are
    // forwarded (--static/--rows/--scroll) and an optional room id opens
    // the Chat tab.
    if (args.command.empty() && args.options.contains("mobile")) {
        matrixcli::cli::Args uiArgs;
        if (!args.positional.empty()) {
            uiArgs.positional.push_back(args.positional[0]);
        }
        uiArgs.options["mobile"] = "true";
        if (args.options.contains("static") || args.options.contains("once") ||
            args.options.contains("print")) {
            uiArgs.options["static"] = "true";
        }
        if (args.options.contains("rows")) uiArgs.options["rows"] = args.options.at("rows");
        if (args.options.contains("scroll")) uiArgs.options["scroll"] = args.options.at("scroll");
        return matrixcli::cmdAsciiUi(uiArgs);
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
            std::cerr << "Usage: progressive-cli td <login|phone|code|password|chats|msg|history>" << std::endl;
            return 1;
        }
        std::string sub = args.positional[0];

        if (sub == "login" || sub == "start") {
            if (!g_tdlib.isAvailable()) g_tdlib.initialize();
            if (!g_tdlib.isAvailable()) { std::cerr << "TDLib not available" << std::endl; return 1; }
            g_tdlib.setTdlibParams(94575, "a3406de8d171bb422bb6ddf3bbd8f4e2");
            std::cout << "TDLib initialized. Run: matrixcli td phone +123****7890" << std::endl;
        } else if (sub == "phone") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td phone +123****7890" << std::endl; return 1; }
            if (!g_tdlib.isAvailable() && !g_tdlib.initialize()) { std::cerr << "TDLib not available" << std::endl; return 1; }
            g_tdlib.sendPhoneNumber(args.positional[1]);
            std::cout << "Code sent. Run: matrixcli td code XXXXX" << std::endl;
        } else if (sub == "code") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td code XXXXX" << std::endl; return 1; }
            g_tdlib.sendAuthCode(args.positional[1]);
            std::cout << "Code sent. If 2FA: matrixcli td password yourpassword" << std::endl;
        } else if (sub == "password" || sub == "2fa") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td password your2fa" << std::endl; return 1; }
            g_tdlib.sendPassword(args.positional[1]);
            std::cout << "2FA sent. Run: matrixcli td chats" << std::endl;
        } else if (sub == "chats") {
            if (g_tdlib.authState() != tdlib::TdAuthState::Ready) { std::cerr << "Not authorized" << std::endl; return 1; }
            auto chats = g_tdlib.getChats(50);
            for (auto& c : chats) {
                std::cout << "  [" << c.id << "] " << c.title << " (" << c.type << ")" << " unread:" << c.unread_count << std::endl;
            }
        } else if (sub == "msg" || sub == "send") {
            if (args.positional.size() < 3) { std::cerr << "Usage: progressive-cli td msg <chat_id> <text>" << std::endl; return 1; }
            int64_t chatId = std::stoll(args.positional[1]);
            std::string text;
            for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) text += " "; text += args.positional[i]; }
            if (g_tdlib.authState() != tdlib::TdAuthState::Ready) { std::cerr << "Not authorized" << std::endl; return 1; }
            g_tdlib.sendMessage(chatId, text);
            std::cout << "Sent to chat " << chatId << std::endl;
        } else if (sub == "history" || sub == "view") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td history <chat_id> [limit]" << std::endl; return 1; }
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
            std::cerr << "Usage: progressive-cli irc <connect|join|msg|leave|whois|names>" << std::endl;
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
            std::cerr << "Usage: progressive-cli lemmy <login|posts|post|upvote|comments>" << std::endl;
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
        if (args.positional.empty()) { std::cerr << "Usage: progressive-cli dc <login|chats|msg|history>" << std::endl; return 1; }
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
              << "Run 'progressive-cli --help' for usage." << std::endl;
    return 1;
}


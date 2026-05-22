#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>

#include "config.hpp"
#include "cli/args.hpp"
#include "server/server.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/logger.hpp"

#ifdef BUILD_TUI
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/main_view.hpp"
#endif

namespace {

std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
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
    matrix::Client client;

    if (!Config::instance().homeserverURL().empty()) {
        client.setHomeserverURL(Config::instance().homeserverURL());
        client.setAccessToken(Config::instance().accessToken());
    }

    server::APIServer api_server(port);
    api_server.start();

    std::cout << "API server running on http://localhost:" << port << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    api_server.stop();
    return 0;
}

int cmdLogin(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    Config::instance().load("config.json");
    matrix::Client client;

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

    client.setHomeserverURL(homeserver);

    try {
        auto token_it = args.options.find("token");
        if (token_it != args.options.end()) {
            auto creds = client.loginToken(token_it->second);
            std::cout << "Logged in as " << creds.user_id << std::endl;
        } else {
            std::string username;
            auto user_it = args.options.find("username");
            if (user_it != args.options.end()) {
                username = user_it->second;
            } else if (args.positional.size() >= 1) {
                username = args.positional[0];
            } else {
                std::cerr << "Error: --username required" << std::endl;
                return 1;
            }

            std::string password;
            auto pass_it = args.options.find("password");
            if (pass_it != args.options.end()) {
                password = pass_it->second;
            } else if (args.positional.size() >= 2) {
                password = args.positional[1];
            } else {
                std::cerr << "Error: --password required" << std::endl;
                return 1;
            }

            auto creds = client.loginPassword(username, password);
            std::cout << "Logged in as " << creds.user_id << std::endl;

            Config::instance().set("homeserver_url", homeserver);
            Config::instance().set("access_token", creds.access_token);
            Config::instance().set("user_id", creds.user_id);
            Config::instance().set("device_id", creds.device_id);
            Config::instance().save();
        }
    } catch (const std::exception& e) {
        std::cerr << "Login failed: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

int cmdStatus(const matrixcli::cli::Args&) {
    using namespace matrixcli;

    Config::instance().load("config.json");

    if (!Config::instance().accessToken().empty()) {
        std::cout << "Logged in as " << Config::instance().userId() << std::endl;
        std::cout << "Homeserver: " << Config::instance().homeserverURL() << std::endl;
        std::cout << "Device ID: " << Config::instance().deviceId() << std::endl;
    } else {
        std::cout << "Not logged in. Use 'matrixcli login' to authenticate." << std::endl;
    }

    return 0;
}

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args&) {
    using namespace matrixcli;

    Config::instance().load("config.json");
    matrix::Client client;

    if (!Config::instance().homeserverURL().empty()) {
        client.setHomeserverURL(Config::instance().homeserverURL());
        client.setAccessToken(Config::instance().accessToken());
    }

    tui::Screen screen;
    screen.init();

    tui::LoginView login_view;
    auto login_result = login_view.run(screen);

    if (login_result.success) {
        try {
            client.setHomeserverURL(login_result.homeserver);
            auto creds = client.loginPassword(login_result.username, login_result.password);
            Config::instance().set("homeserver_url", login_result.homeserver);
            Config::instance().set("access_token", creds.access_token);
            Config::instance().set("user_id", creds.user_id);
            Config::instance().set("device_id", creds.device_id);
            Config::instance().save();

            tui::MainView main_view;
            main_view.setStatus("Connected as " + creds.user_id);

            client.startSync([&](const matrix::Event& ev) {
                if (ev.type == "m.room.message" && ev.content.contains("body") &&
                    !ev.content["body"].get<std::string>().empty()) {
                    main_view.addMessage(ev.sender, ev.content["body"].get<std::string>());
                }
            });

            main_view.run(screen);
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

} // anonymous namespace

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    auto args = matrixcli::cli::parseArgs(argc, argv);

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

    if (args.command == "status") {
        return cmdStatus(args);
    }

    if (args.command == "send") {
        std::cerr << "send: not implemented via CLI yet. Use the API server or TUI." << std::endl;
        return 1;
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

    std::cerr << "Unknown command: " << args.command << "\n"
              << "Run 'matrixcli --help' for usage." << std::endl;
    return 1;
}

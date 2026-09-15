// gomuks_commands.cpp — frontend for a gomuks backend over its websocket
// + jsoncmd API (experimental, M1 surface: auth, ping, get_state).
//
// The backend (go.mau.fi/gomuks) owns the Matrix session and the E2EE
// keys; this CLI only renders and drives it:
//
//   progressive-cli gomuks auth --base-url URL --username U [--password P]
//   progressive-cli gomuks ping [--timeout N]
//   progressive-cli gomuks state [--timeout N] [--json]
//
// The session (base URL + gomuks_auth cookie) persists in config.json,
// same threat model as the stored Matrix access tokens: plaintext on
// disk, readable by the local user only.
#include "commands.hpp"
#include "config.hpp"

#include <iostream>
#include <termios.h>
#include <unistd.h>

#include "../lib/gomuks/gomuks_client.hpp"

using namespace matrixcli;

namespace {

std::string promptHidden(const std::string& label) {
    std::cout << label << std::flush;
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::string value;
    std::getline(std::cin, value);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << std::endl;
    return value;
}

int cmdGomuksAuth(const cli::Args& args) {
    std::string base =
        args.options.count("base-url") ? args.options.at("base-url") : "";
    std::string user =
        args.options.count("username") ? args.options.at("username") : "";
    std::string pass =
        args.options.count("password") ? args.options.at("password") : "";
    if (args.options.count("interactive")) {
        if (base.empty()) {
            std::cout << "Backend base URL (e.g. http://127.0.0.1:8080): "
                      << std::flush;
            std::getline(std::cin, base);
        }
        if (user.empty()) {
            std::cout << "Backend username: " << std::flush;
            std::getline(std::cin, user);
        }
        if (pass.empty()) pass = promptHidden("Backend password: ");
    }
    if (base.empty() || user.empty() || pass.empty()) {
        std::cerr << "Usage: progressive-cli gomuks auth --base-url URL "
                     "--username U --password P [--interactive]"
                  << std::endl;
        return 1;
    }
    gomuks::GomuksClient backend;
    std::string error;
    if (!backend.login(base, user, pass, error)) {
        std::cerr << "gomuks auth failed: " << error << std::endl;
        return 1;
    }
    Config::instance().load("config.json");
    Config::instance().set("gomuks_base_url", base);
    // The cookie is a bearer secret: config.json must stay user-readable
    // only (same rule as the stored Matrix access tokens).
    Config::instance().set("gomuks_cookie", backend.authCookie());
    Config::instance().save();
    if (backend.authCookie().empty()) {
        std::cerr << "gomuks auth failed: empty session cookie" << std::endl;
        return 1;
    }
    std::cout << "Authenticated to the gomuks backend at " << base
              << " (session stored in config.json)" << std::endl;
    return 0;
}

} // namespace

int cmdGomuks(const cli::Args& args) {
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli gomuks <auth|ping|state>"
                  << std::endl;
        return 1;
    }
    const std::string sub = args.positional[0];
    if (sub == "auth") return cmdGomuksAuth(args);

    int timeout = 15;
    if (args.options.count("timeout")) {
        try {
            timeout = std::stoi(args.options.at("timeout"));
        } catch (...) {}
        if (timeout <= 0) timeout = 15;
    }
    Config::instance().load("config.json");
    const std::string base = Config::instance().get("gomuks_base_url", "");
    const std::string cookie = Config::instance().get("gomuks_cookie", "");
    if (base.empty() || cookie.empty()) {
        std::cerr << "No gomuks session stored — run 'gomuks auth' first."
                  << std::endl;
        return 1;
    }
    gomuks::GomuksClient backend;
    backend.setSession(base, cookie);
    std::string error;
    if (!backend.connect(timeout, error)) {
        std::cerr << "gomuks connect failed: " << error << std::endl;
        return 1;
    }
    if (sub == "ping") {
        if (!backend.ping(0, timeout, error)) {
            std::cerr << "gomuks ping failed: " << error << std::endl;
            return 1;
        }
        std::cout << "pong (" << base << ")" << std::endl;
        return 0;
    }
    if (sub == "state") {
        nlohmann::json state;
        if (!backend.getState(state, timeout, error)) {
            std::cerr << "gomuks state failed: " << error << std::endl;
            return 1;
        }
        if (args.options.count("json")) {
            std::cout << state.dump(2) << std::endl;
        } else {
            std::cout << "logged in: "
                      << (state.value("is_logged_in", false) ? "yes" : "no")
                      << std::endl;
            if (state.contains("user_id"))
                std::cout << "user: " << state.value("user_id", "")
                          << std::endl;
            if (state.contains("device_id"))
                std::cout << "device: " << state.value("device_id", "")
                          << std::endl;
        }
        return 0;
    }
    std::cerr << "Unknown gomuks subcommand: " << sub
              << " (auth|ping|state)" << std::endl;
    return 1;
}

void registerGomuksCommands() {
    CommandRegistry::instance().registerCli(
        "gomuks", cmdGomuks,
        "Gomuks backend frontend (experimental): gomuks auth|ping|state");
}

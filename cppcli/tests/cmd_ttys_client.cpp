// progressive-cli ttys — thin client for `serve --ttys`.
//
// Runs under ANY local user (root, another machine's account, …): sends the
// terminal size + the account (from config.json or explicit flags) to the
// remote serve --ttys port, and prints the ASCII frame it gets back. The
// account is passed per request; the serve side never stores it.

#include "commands.hpp"
#include "main_commands.hpp"
#include "cli/args.hpp"
#include "config.hpp"
#include "ascii_ui_impl.hpp"
#include "../lib/http/http.hpp"
#include "../lib/util/string_utils.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

using json = nlohmann::json;

namespace matrixcli {

static int localTerminalWidth() {
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return static_cast<int>(ws.ws_col);
#endif
    return terminalColumns();
}

static int localTerminalHeight() {
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return static_cast<int>(ws.ws_row);
#endif
    return 24 + 5;
}

int cmdTtys(const cli::Args& args) {
    std::string host = "127.0.0.1";
    std::string port = "8080";
    if (args.options.count("host")) host = args.options.at("host");
    if (args.options.count("port")) port = args.options.at("port");
    if (args.options.count("p")) port = args.options.at("p");

    Config::instance().load("config.json");
    std::string hs = args.options.count("homeserver") ? args.options.at("homeserver")
                                                      : Config::instance().get("homeserver_url", "");
    std::string tok = args.options.count("access-token") ? args.options.at("access-token")
                                                         : Config::instance().get("access_token", "");
    std::string uid = args.options.count("user") ? args.options.at("user")
                                                 : Config::instance().get("user_id", "");
    std::string dev = args.options.count("device") ? args.options.at("device")
                                                   : Config::instance().get("device_id", "");
    if (hs.empty() || tok.empty()) {
        std::cerr << "ttys: account required (--homeserver + --access-token), "
                     "or a config.json with homeserver_url/access_token.\n";
        return 1;
    }

    std::string base = "http://" + host + ":" + port;
    std::string token = args.options.count("token") ? args.options.at("token") : "";
    std::map<std::string, std::string> headers;
    if (!token.empty()) headers["Authorization"] = "Bearer " + token;

    // Open/reuse the session. The server keys sessions by user_id.
    json account;
    account["homeserver"] = hs;
    account["access_token"] = tok;
    account["user_id"] = uid;
    account["device_id"] = dev;
    json sessReq;
    sessReq["account"] = account;
    std::string sessId;
    {
        http::Client http;
        auto resp = http.post(base + "/api/ttys/session", sessReq.dump(), headers);
        if (!resp.ok()) {
            std::cerr << "ttys: session failed (HTTP " << resp.status_code << "): "
                      << resp.body << "\n";
            return 1;
        }
        json j = json::parse(resp.body, nullptr, false);
        if (j.is_discarded() || !j.contains("session")) {
            std::cerr << "ttys: bad session response: " << resp.body << "\n";
            return 1;
        }
        sessId = j["session"].get<std::string>();
    }

    json req;
    req["session"] = sessId;
    req["cols"] = localTerminalWidth();
    req["rows"] = localTerminalHeight();
    std::string sync = "off";
    if (args.options.count("sync")) sync = args.options.at("sync");
    req["sync"] = sync;

    auto render = [&](const std::string& input) -> std::string {
        json r = req;
        std::string path = "/api/ttys/input";
        if (!input.empty()) r["input"] = input;
        else path = "/api/ttys/render";
        http::Client http;
        auto resp = http.post(base + path, r.dump(), headers);
        if (!resp.ok()) return "ttys: render failed (HTTP " + std::to_string(resp.status_code)
                                      + "): " + resp.body;
        json j = json::parse(resp.body, nullptr, false);
        return j.is_discarded() || !j.contains("frame") ? "ttys: bad frame"
                                                        : j["frame"].get<std::string>();
    };

    std::cout << render("") << std::flush;

    // Interactive: send REPL-style lines and print the next frame. The
    // server keeps the UiState per session, so "open X", "find q", etc.
    std::string line;
    while (args.positional.empty()) {
        std::cout << "ttys> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) { std::cout << render("") << std::flush; continue; }
        auto e = line.find_last_not_of(" \t");
        line = line.substr(b, e - b + 1);
        if (line == "quit" || line == "exit" || line == "q") break;
        std::cout << render(line) << std::flush;
    }
    return 0;
}

} // namespace matrixcli

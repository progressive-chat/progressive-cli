// src/room_commands.cpp — session/room/API commands on the vendored desktop
// core (lib/ecore): one-shot sync, device management, moderation, profiles,
// threads, public room directory. Registered via CommandRegistry.
#include "commands.hpp"
#include "pcore.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "ascii_ui_impl.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/string_utils.hpp"
#include <progressive/markdown.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

using namespace matrixcli;
// ---- Discovery / token / account-data inspection commands ----

// Build a wrapper client from the persisted session (same pattern as
// bridge_commands.cpp) so the matrixcli::matrix::Client-only helpers
// (getTurnServer, getOpenIdToken, …) are usable here.
namespace { void configureSessionClient(matrixcli::matrix::Client& client) {
    matrixcli::db::Database dbi;
    if (dbi.open("matrixcli.db")) {
        auto acc = dbi.loadAccount();
        if (acc.is_logged_in()) {
            client.setHomeserverURL(acc.homeserver_url);
            client.setAccessToken(acc.access_token);
            client.setUserId(acc.user_id);
        }
    }
} }

// TURN credentials for VoIP (GET /voip/turnServer). Clients use these as ICE
// servers so 1:1 and group calls connect through NAT / symmetric firewalls.
int cmdTurn(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    matrixcli::matrix::Client client;
    configureSessionClient(client);
    auto j = client.getTurnServer();
    if (j.is_null() || j.empty()) { std::cerr << "No TURN server advertised.\n"; return 1; }
    if (args.options.count("json")) { std::cout << j.dump() << "\n"; return 0; }
    std::cout << "TURN credentials (ttl " << j.value("ttl", 0) << "s):\n";
    if (j.contains("username")) std::cout << "  username: " << j["username"].get<std::string>() << "\n";
    if (j.contains("password")) std::cout << "  password: " << j["password"].get<std::string>() << "\n";
    if (j.contains("uris") && j["uris"].is_array()) {
        for (auto& u : j["uris"]) std::cout << "  uri: " << u.get<std::string>() << "\n";
    }
    return 0;
}

// OpenID token (POST /user/{userId}/openid/request_token) — identifies the
// Matrix user to third-party services (widgets, integrations, OIDC bridges).
int cmdOpenId(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    matrixcli::matrix::Client client;
    configureSessionClient(client);
    auto j = client.getOpenIdToken();
    if (j.is_null() || j.empty()) { std::cerr << "OpenID token request failed.\n"; return 1; }
    std::cout << (args.options.count("json") ? j.dump() : j.dump(2)) << "\n";
    return 0;
}

// Server capabilities (GET /capabilities) — which room versions it can create,
// whether e2ee is enforced, etc.
int cmdCapabilities(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    matrixcli::matrix::Client client;
    configureSessionClient(client);
    auto j = client.getCapabilities();
    if (j.is_null() || j.empty()) { std::cerr << "Capabilities request failed.\n"; return 1; }
    std::cout << (args.options.count("json") ? j.dump() : j.dump(2)) << "\n";
    return 0;
}

// Third-party network directory and lookups (GET /thirdparty/*).
int cmdThirdparty(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    matrixcli::matrix::Client client;
    configureSessionClient(client);
    if (args.positional.empty() || args.positional[0] == "protocols") {
        auto j = client.getThirdpartyProtocols();
        std::cout << (args.options.count("json") ? j.dump() : j.dump(2)) << "\n";
        return 0;
    }
    const std::string& proto = args.positional[0];
    std::string net = args.options.count("network") ? args.options.at("network") : "";
    nlohmann::json j;
    if (args.positional.size() > 1 && args.positional[1] == "users")
        j = client.getThirdpartyUsers(proto, net);
    else if (args.positional.size() > 1 && args.positional[1] == "locations")
        j = client.getThirdpartyLocations(proto, net);
    else
        j = client.getThirdpartyUsers(proto, net);
    std::cout << (args.options.count("json") ? j.dump() : j.dump(2)) << "\n";
    return 0;
}

int cmdLink(const cli::Args& args) {
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli link <room> [ref|last|first|N|-N] "
                     "[--last] [--first] [--n N] [--from-end N] [--event $id] [--via N]\n";
        return 1;
    }
    std::string roomQ = args.positional[0];
    std::string ref;
    bool hasFlag = false;
    if (args.options.count("last")) { ref = "last"; hasFlag = true; }
    else if (args.options.count("first")) { ref = "first"; hasFlag = true; }
    else if (args.options.count("event")) { ref = args.options.at("event"); hasFlag = true; }
    else if (args.options.count("n")) { ref = args.options.at("n"); hasFlag = true; }
    else if (args.options.count("from-end")) { ref = "-" + args.options.at("from-end"); hasFlag = true; }
    if (!hasFlag && args.positional.size() >= 2) ref = args.positional[1];
    if (ref.empty()) ref = "last";

    int viaLimit = 3;
    if (args.options.count("via")) {
        try { viaLimit = std::stoi(args.options.at("via")); } catch (...) {}
    }

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) { std::cerr << "Cannot open database (matrixcli.db)\n"; return 1; }
    std::string roomId = matchRoomInCache(dbi.listRooms(), roomQ);
    if (roomId.empty()) roomId = roomQ;

    matrix::Event ev;
    bool found = false;
    if (ref == "last") {
        auto evs = dbi.getEvents(roomId, 500);
        if (!evs.empty()) { ev = evs.front(); found = true; }
    } else if (ref == "first") {
        auto evs = dbi.getEvents(roomId, 100000);
        if (!evs.empty()) { ev = evs.back(); found = true; }
    } else if (!ref.empty() && ref[0] == '-') {
        int n = 0;
        try { n = -std::stoi(ref); } catch (...) { n = 0; }
        if (n >= 1) {
            auto evs = dbi.getEvents(roomId, std::max(n, 500));
            if ((int)evs.size() >= n) { ev = evs[n - 1]; found = true; }
        }
    } else if (!ref.empty() && (ref[0] == '+' || (ref[0] >= '0' && ref[0] <= '9'))) {
        long n = 0;
        try { n = (ref[0] == '+') ? std::stol(ref.substr(1)) : std::stol(ref); }
        catch (...) { n = 0; }
        if (n >= 1) {
            auto evs = dbi.getEvents(roomId, 100000);
            if ((int)evs.size() >= n) { ev = evs[evs.size() - n]; found = true; }
        }
    } else {
        found = dbi.getEventById(ref, ev);
    }
    if (!found) {
        std::cerr << "Event not found in the cache for ref '" << ref << "'.\n";
        return 1;
    }
    std::cout << "https://matrix.to/#/" << roomId << "/" << ev.event_id
              << viaSuffix(&dbi, roomId, viaLimit) << std::endl;
    return 0;
}

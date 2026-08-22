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
#include <cstdio>

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

// Clipboard helpers: copy to / read from the system clipboard via the common
// X11/Wayland/macOS utilities. Best-effort — returns false / "" if none is
// installed (e.g. a headless box).
static bool copyToClipboard(const std::string& text) {
    const char* tools[] = {"wl-copy", "xclip -selection clipboard",
                           "xsel --clipboard --input", "pbcopy", nullptr};
    for (int i = 0; tools[i]; ++i) {
        std::string cmd = std::string(tools[i]) + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "w");
        if (!p) continue;
        fwrite(text.data(), 1, text.size(), p);
        if (pclose(p) == 0) return true;
    }
    return false;
}

static std::string readClipboard() {
    const char* tools[] = {"wl-paste", "xclip -selection clipboard -o",
                           "xsel --clipboard --output", "pbpaste", nullptr};
    for (int i = 0; tools[i]; ++i) {
        std::string cmd = std::string(tools[i]) + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) continue;
        std::string out; char buf[1024]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        pclose(p);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                                out.back() == ' ')) out.pop_back();
        if (!out.empty()) return out;
    }
    return "";
}

int cmdLink(const cli::Args& args) {
    // Optional: take the room from the system clipboard (--clip).
    std::string clipRoom;
    if (args.options.count("clip")) {
        clipRoom = readClipboard();
        if (clipRoom.empty()) {
            std::cerr << "Clipboard is empty or no clipboard tool available.\n";
            return 1;
        }
    }

    // Per-link domain (config link_domain, overridable with --domain). Defaults
    // to matrix.to. Used to build the permalink base.
    std::string linkDomain = "matrix.to";
    {
        Config::instance().load("config.json");
        linkDomain = Config::instance().get("link_domain", "matrix.to");
    }
    if (args.options.count("domain")) linkDomain = args.options.at("domain");
    std::string linkBase;
    if (linkDomain.find("://") != std::string::npos) linkBase = linkDomain;
    else linkBase = "https://" + linkDomain;
    if (!linkBase.empty() && linkBase.back() != '/') linkBase += '/';
    linkBase += "#/";

    // Print the link, or copy it to the clipboard with --copy.
    auto outputLink = [&](const std::string& link) {
        if (args.options.count("copy")) {
            if (copyToClipboard(link))
                std::cout << "Copied to clipboard: " << link << "\n";
            else {
                std::cerr << "Clipboard tool not found; printing the link:\n";
                std::cout << link << "\n";
            }
        } else {
            std::cout << link << "\n";
        }
    };

    if (args.positional.empty() && clipRoom.empty()) {
        // No room given: default to the most recently active room (the one
        // whose last message is newest) and link its latest event, then
        // remind the user of the other forms.
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) {
            std::cerr << "Cannot open database (matrixcli.db)\n";
            return 1;
        }
        auto rooms = dbi.listRooms();
        std::string bestRoom, bestEvent;
        long long bestTs = -1;
        for (const auto& r : rooms) {
            std::string rid = r.value("room_id", "");
            if (rid.empty()) continue;
            auto evs = dbi.getEvents(rid, 1);
            if (!evs.empty() && evs.front().origin_server_ts > bestTs) {
                bestTs = evs.front().origin_server_ts;
                bestRoom = rid;
                bestEvent = evs.front().event_id;
            }
        }
        if (bestRoom.empty()) {
            std::cerr << "No rooms with messages in the cache.\n";
            std::cerr << "Usage: progressive-cli link <room> [last|first|N|-N] "
                         "[--last] [--first] [--n N] [--from-end N] [--event $id] "
                         "[--via N] [--copy] [--clip]\n";
            return 1;
        }
        std::string name = bestRoom;
        for (const auto& r : rooms)
            if (r.value("room_id", "") == bestRoom) { name = r.value("name", bestRoom); break; }
        std::cout << "Last active room: " << name << "\n";
        outputLink(linkBase + bestRoom + "/" + bestEvent +
                   viaSuffix(&dbi, bestRoom, 3));
        std::cout << "Other forms: link <room> [last|first|N|-N] [--via N] "
                     "(permalink is an alias; --event $id for a specific event; --copy/--clip)\n";
        return 0;
    }

    std::string roomQ = args.positional.empty() ? clipRoom : args.positional[0];
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
    outputLink(linkBase + roomId + "/" + ev.event_id +
               viaSuffix(&dbi, roomId, viaLimit));
    return 0;
}

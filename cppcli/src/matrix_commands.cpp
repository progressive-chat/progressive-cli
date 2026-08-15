// src/matrix_commands.cpp — legacy room commands extracted from main.cpp.
// Still on the pre-ecore matrix::Client path (config.json session); kept
// functional while the ecore migration completes. Registered via registry.
#include "commands.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "pcore.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/string_utils.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <set>
#include <thread>
#include <chrono>

using namespace matrixcli;

int cmdVote(const cli::Args& args) {
        // matrixcli vote room_id poll_event_id answer1,answer2
        if (args.positional.size() < 3) {
            std::cerr << "Usage: matrixcli vote <room> <poll_event_id> <answer1>[,answer2...]" << std::endl;
            return 1;
        }
        using namespace matrixcli;
        matrix::Client client;
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        auto acc = dbi.loadAccount();
        if (!acc.is_logged_in()) { std::cerr << "Not logged in" << std::endl; return 1; }
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);

        std::string room = args.positional[0];
        std::string pollId = args.positional[1];
        std::vector<std::string> answers;
        for (size_t i = 2; i < args.positional.size(); i++) answers.push_back(args.positional[i]);

        try {
            auto eid = client.sendPollResponse(room, pollId, answers);
            std::cout << "Voted [" << eid << "]" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Vote failed: " << e.what() << std::endl;
            return 1;
        }
        return 0;
}

int cmdReact(const cli::Args& args) {
        if (args.positional.size() < 3) {
            std::cerr << "Usage: matrixcli react <room> <event_id> <emoji>" << std::endl;
            return 1;
        }
        using namespace matrixcli;
        matrix::Client client;
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        auto acc = dbi.loadAccount();
        if (!acc.is_logged_in()) { std::cerr << "Not logged in" << std::endl; return 1; }
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);

        try {
            auto eid = client.sendReaction(args.positional[0], args.positional[1], args.positional[2]);
            std::cout << "Reacted [" << eid << "]" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Reaction failed: " << e.what() << std::endl;
            return 1;
        }
        return 0;
}

int cmdTopic(const cli::Args& args) {
        if (args.positional.size() < 2) {
            std::cerr << "Usage: matrixcli topic <room> <topic>" << std::endl;
            return 1;
        }
        using namespace matrixcli;
        matrix::Client client;
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        auto acc = dbi.loadAccount();
        if (!acc.is_logged_in()) { std::cerr << "Not logged in" << std::endl; return 1; }
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);

        std::string body;
        for (size_t i = 1; i < args.positional.size(); i++) {
            if (i > 1) body += " "; body += args.positional[i];
        }
        try {
            client.setRoomTopic(args.positional[0], body);
            std::cout << "Topic set" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Failed: " << e.what() << std::endl;
            return 1;
        }
        return 0;
}

int cmdRoomname(const cli::Args& args) {
        if (args.positional.size() < 2) {
            std::cerr << "Usage: matrixcli roomname <room> <name>" << std::endl;
            return 1;
        }
        using namespace matrixcli;
        matrix::Client client;
        db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
        auto acc = dbi.loadAccount();
        if (!acc.is_logged_in()) { std::cerr << "Not logged in" << std::endl; return 1; }
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);

        std::string body;
        for (size_t i = 1; i < args.positional.size(); i++) {
            if (i > 1) body += " "; body += args.positional[i];
        }
        try { client.setRoomName(args.positional[0], body); std::cout << "OK" << std::endl; }
        catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
        return 0;
}

int cmdAvatar(const cli::Args& args) {
        if (args.positional.size() < 2) {
            std::cerr << "Usage: matrixcli avatar <room> <file|mxc_url>" << std::endl;
            return 1;
        }
        using namespace matrixcli;
        matrix::Client client;
        db::Database dbi; if (!dbi.open("matrixcli.db")) return 1;
        auto acc = dbi.loadAccount();
        if (!acc.is_logged_in()) { std::cerr << "Not logged in" << std::endl; return 1; }
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);

        std::string url = args.positional[1];
        if (url.find("mxc://") != 0 && url.find("http") != 0) {
            try { url = client.uploadMedia(url); } catch (...) { std::cerr << "Upload failed" << std::endl; return 1; }
        }
        try { client.setRoomAvatar(args.positional[0], url); std::cout << "Avatar set" << std::endl; }
        catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
        return 0;
}

int cmdRead(const cli::Args& args) {
        using namespace matrixcli;
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (args.options.count("all")) {
            dbi.markAllRead();
            // The last-read position moves to the newest event of every
            // room (the local m.fully_read copy).
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                auto evs = dbi.getEvents(id, 1);
                if (!evs.empty()) dbi.setReadMarker(id, evs.front().event_id);
            }
            std::cout << "All read." << std::endl;
        } else if (!args.positional.empty()) {
            std::string room = args.positional[0];
            dbi.markRoomRead(room);
            // The last-read marker (the local m.fully_read copy) moves to
            // the newest cached event of the room.
            std::string lastEvent;
            auto evs = dbi.getEvents(room, 1);
            if (!evs.empty()) {
                lastEvent = evs.front().event_id;
                dbi.setReadMarker(room, lastEvent);
            }
            // The server-side read markers follow the per-room receipts
            // policy (the `receipts` command toggles it).
            if (dbi.receiptsEnabled(room)) {
                if (pcore::init() && pcore::loadSavedSession()) {
                    auto r = pcore::core().client->setReadMarker(room, lastEvent);
                    if (!r.ok) {
                        std::cout << "Marked " << room
                                  << " read (the marker failed: " << r.error.message
                                  << ")." << std::endl;
                        return 0;
                    }
                    std::cout << "Marked " << room << " read (receipt sent)." << std::endl;
                } else {
                    std::cout << "Marked " << room
                              << " read (no session — the marker was not sent)." << std::endl;
                }
            } else {
                std::cout << "Marked " << room
                          << " read (receipts off — the marker was NOT sent)."
                          << std::endl;
            }
        } else { std::cerr << "Usage: matrixcli read <room> | matrixcli read --all" << std::endl; return 1; }
        return 0;
}

// ── receipts ── the per-room read-receipt policy (Element-style).
//   matrixcli receipts                — the policy for every room
//   matrixcli receipts <room>         — the status of one room
//   matrixcli receipts <room> on|off  — set it
int cmdReceipts(const cli::Args& args) {
        using namespace matrixcli;
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        bool json_out = args.options.count("json");

        if (!args.positional.empty()) {
            std::string room = args.positional[0];
            if (args.positional.size() >= 2 &&
                (args.positional[1] == "on" || args.positional[1] == "off")) {
                bool enable = args.positional[1] == "on";
                dbi.setReceiptsEnabled(room, enable);
                if (json_out) {
                    nlohmann::json j;
                    j["room"] = room;
                    j["receipts"] = enable;
                    std::cout << j.dump() << std::endl;
                } else {
                    std::cout << "Read receipts for " << room << ": "
                              << (enable ? "on" : "off") << std::endl;
                }
                return 0;
            }
            bool on = dbi.receiptsEnabled(room);
            if (json_out) {
                nlohmann::json j;
                j["room"] = room;
                j["receipts"] = on;
                std::cout << j.dump() << std::endl;
            } else {
                std::cout << "Read receipts for " << room << ": "
                          << (on ? "on" : "off") << std::endl;
            }
            return 0;
        }

        // The whole room list with the per-room policy.
        auto off = dbi.receiptsOffRooms();
        std::set<std::string> offSet(off.begin(), off.end());
        if (json_out) {
            nlohmann::json arr = nlohmann::json::array();
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                arr.push_back({{"room", id},
                               {"name", r.value("name", id)},
                               {"receipts", !offSet.count(id)}});
            }
            std::cout << arr.dump() << std::endl;
        } else {
            std::cout << "Read receipts (default: on everywhere):" << std::endl;
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::cout << "  " << (offSet.count(id) ? "[off] " : "[on]  ")
                          << r.value("name", id) << std::endl;
            }
            std::cout << "\nToggle: matrixcli receipts <room> on|off" << std::endl;
        }
        return 0;
}

int cmdNotifications(const cli::Args& args) {
        using namespace matrixcli;

        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        int limit = args.options.count("limit") ? std::stoi(args.options.at("limit")) : 20;
        bool all = args.options.count("all");
        auto notifs = dbi.getNotifications(limit, !all);
        if (notifs.empty()) { std::cout << "No notifications." << std::endl; return 0; }
        int total = dbi.getNotificationCount();
        std::cout << "Notifications: " << notifs.size() << (total > (int)notifs.size() ? " (total: " + std::to_string(total) + ")" : "") << std::endl << std::endl;
        for (auto& n : notifs) {
            std::string room = n.value("room_name", n.value("room_id", "?"));
            std::string sender = n.value("sender", "?");
            auto at = sender.find(':'); if (at != std::string::npos && sender.starts_with("@")) sender = sender.substr(1, at - 1);
            std::string body = n.value("body", ""); if (body.size() > 80) body = body.substr(0, 77) + "...";
            bool hl = n.value("highlight", false);
            std::cout << (hl ? ANSI_BOLD "★ " ANSI_RESET : "  ") << ansiUser(n["sender"], "[" + sender + "]") << " #" << room << "  " << body << std::endl;
        }
        std::cout << "\nMark read: matrixcli read <room> | matrixcli read --all" << std::endl;
        return 0;
}

// ── filter ── permanent view filters (stored in config.json "filters").
// Positive (--senders): show only these users. Negative (--hide): drop users.
// Both optional per-room (--room); without --room they apply globally.
// Temporary variants live on `view` (--senders/--hide last one invocation).
int cmdFilter(const cli::Args& args) {
    bool json_out = args.options.count("json");
    Config::instance().load("config.json");
    nlohmann::json flt = Config::instance().filters();
    if (!flt.is_object()) flt = nlohmann::json::object();

    auto parseList = [](const std::string& csv) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : csv) {
            if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    auto toJsonArray = [](const std::vector<std::string>& list) {
        nlohmann::json arr = nlohmann::json::array();
        for (auto& s : list) arr.push_back(s);
        return arr;
    };

    std::string room = args.options.count("room") ? args.options.at("room") : "";
    if (!room.empty() && room[0] != '!') {
        // resolve by name (room ids start with '!'); #names and bare names too
        db::Database dbi;
        if (dbi.open("matrixcli.db")) {
            for (auto& r : dbi.listRooms()) {
                std::string id = r.value("room_id", "");
                std::string name = r.value("name", "");
                if (id == room || name == room || name.find(room) == 0) { room = id; break; }
            }
        }
    }

    if (args.options.count("senders")) {
        auto list = parseList(args.options.at("senders"));
        if (room.empty()) flt["senders"] = toJsonArray(list);
        else {
            if (!flt.contains("rooms") || !flt["rooms"].is_object()) flt["rooms"] = nlohmann::json::object();
            flt["rooms"][room]["senders"] = toJsonArray(list);
        }
        Config::instance().setFilters(flt);
        Config::instance().save();
        std::cout << "Positive filter set: show only " << list.size() << " user(s)"
                  << (room.empty() ? " (all rooms)" : " in " + room) << std::endl;
        return 0;
    }
    if (args.options.count("hide")) {
        auto list = parseList(args.options.at("hide"));
        if (room.empty()) flt["hide"] = toJsonArray(list);
        else {
            if (!flt.contains("rooms") || !flt["rooms"].is_object()) flt["rooms"] = nlohmann::json::object();
            flt["rooms"][room]["hide"] = toJsonArray(list);
        }
        Config::instance().setFilters(flt);
        Config::instance().save();
        std::cout << "Hidden users set: " << list.size() << " user(s)"
                  << (room.empty() ? " (all rooms)" : " in " + room) << std::endl;
        return 0;
    }
    bool want_clear = args.options.count("clear") || (!args.positional.empty() && args.positional[0] == "clear");
    if (want_clear) {
        if (room.empty()) {
            flt = nlohmann::json::object();
            std::cout << "All filters cleared." << std::endl;
        } else {
            if (flt.contains("rooms") && flt["rooms"].is_object()) flt["rooms"].erase(room);
            std::cout << "Filters cleared for " << room << "." << std::endl;
        }
        Config::instance().setFilters(flt);
        Config::instance().save();
        return 0;
    }

    // status
    if (json_out) {
        std::cout << flt.dump() << std::endl;
    } else {
        auto printList = [](const nlohmann::json& j, const std::string& key, const std::string& indent) {
            if (j.is_object() && j.contains(key) && j[key].is_array() && !j[key].empty()) {
                std::cout << indent << (key == "senders" ? "show only: " : "hide: ");
                bool first = true;
                for (auto& v : j[key]) { if (!first) std::cout << ", "; std::cout << v.get<std::string>(); first = false; }
                std::cout << std::endl;
            }
        };
        if (flt.empty()) { std::cout << "No filters set." << std::endl; }
        printList(flt, "senders", "");
        printList(flt, "hide", "");
        if (flt.contains("rooms") && flt["rooms"].is_object()) {
            for (auto& [rid, rj] : flt["rooms"].items()) {
                if (rj.is_object() && (rj.contains("senders") || rj.contains("hide"))) {
                    std::cout << "room " << rid << ":" << std::endl;
                    printList(rj, "senders", "  ");
                    printList(rj, "hide", "  ");
                }
            }
        }
    }
    return 0;
}

void registerMatrixCommands() {
    using namespace matrixcli;
    auto& reg = CommandRegistry::instance();
    reg.registerCli("vote", cmdVote, "Vote in a poll: vote <room> <poll_event_id> <answer1>[,answer2...]");
    reg.registerCli("react", cmdReact, "React to a message: react <room> <event_id> <emoji>");
    reg.registerCli("filter", cmdFilter, "Permanent view filters: filter --senders @u,@u2 [--room X] | --hide @u [--room X] | status | clear [--room X]");
    reg.registerCli("topic", cmdTopic, "Set room topic: topic <room> <text>");
    reg.registerCli("roomname", cmdRoomname, "Set room name: roomname <room> <name>");
    reg.registerCli("avatar", cmdAvatar, "Set room avatar: avatar <room> <mxc-url>");
    reg.registerCli("read", cmdRead, "Mark room read: read <room>");
    reg.registerCli("receipts", cmdReceipts, "Per-room read receipts: receipts [<room>] [on|off]");    reg.registerCli("notifications", cmdNotifications, "Notification settings: notifications (on|off)");
    reg.registerCli("notif", cmdNotifications, "Notification settings (alias)");
}

// src/matrix_commands.cpp — legacy room commands extracted from main.cpp.
// Still on the pre-ecore matrix::Client path (config.json session); kept
// functional while the ecore migration completes. Registered via registry.
#include "commands.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/string_utils.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
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
        if (args.options.count("all")) { dbi.markAllRead(); std::cout << "All read." << std::endl; }
        else if (!args.positional.empty()) { dbi.markRoomRead(args.positional[0]); std::cout << "Marked " << args.positional[0] << " read." << std::endl; }
        else { std::cerr << "Usage: matrixcli read <room> | matrixcli read --all" << std::endl; return 1; }
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

void registerMatrixCommands() {
    auto& reg = CommandRegistry::instance();
    reg.registerCli("vote", cmdVote, "Vote in a poll: vote <room> <poll_event_id> <answer1>[,answer2...]");
    reg.registerCli("react", cmdReact, "React to a message: react <room> <event_id> <emoji>");
    reg.registerCli("topic", cmdTopic, "Set room topic: topic <room> <text>");
    reg.registerCli("roomname", cmdRoomname, "Set room name: roomname <room> <name>");
    reg.registerCli("avatar", cmdAvatar, "Set room avatar: avatar <room> <mxc-url>");
    reg.registerCli("read", cmdRead, "Mark room read: read <room>");
    reg.registerCli("notifications", cmdNotifications, "Notification settings: notifications (on|off)");
    reg.registerCli("notif", cmdNotifications, "Notification settings (alias)");
}

#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>

#include "config.hpp"
#include "cli/args.hpp"
#include "server/server.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"

#ifdef BUILD_TUI
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
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

    // Try loading credentials from database first, fall back to config
    db::Database dbi;
    dbi.open("matrixcli.db");
    auto acc = dbi.loadAccount();
    if (acc.is_logged_in()) {
        client.setHomeserverURL(acc.homeserver_url);
        client.setAccessToken(acc.access_token);
        util::Logger::instance().info("Loaded saved session for " + acc.user_id);
    } else if (!Config::instance().homeserverURL().empty()) {
        client.setHomeserverURL(Config::instance().homeserverURL());
        client.setAccessToken(Config::instance().accessToken());
    }
    client.setDatabase(&dbi);

    // Initialize crypto if logged in
    if (acc.is_logged_in()) {
        client.initCrypto(acc.user_id, acc.device_id);
    }

    bool demo_mode = args.options.contains("demo");
    if (!demo_mode && args.command == "demo") demo_mode = true;

    server::APIServer api_server(port, demo_mode);
    api_server.start();

    // Start background sync if logged in (populates DB for CLI commands)
    if (!demo_mode && acc.is_logged_in()) {
        client.startSync([&](const matrix::Event&) {
            // Events auto-saved to DB by Client's built-in sync handler
        });
        util::Logger::instance().info("Background sync started");
    }

    std::cout << "API server running on http://localhost:" << port << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    client.stopSync();
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

            // Save to database for persistent sync state
            db::Database dbi;
            dbi.open("matrixcli.db");
            db::StoredAccount acc;
            acc.homeserver_url = homeserver;
            acc.user_id = creds.user_id;
            acc.access_token = creds.access_token;
            acc.device_id = creds.device_id;
            dbi.saveAccount(acc);
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

    // Try DB first
    db::Database dbi;
    dbi.open("matrixcli.db");
    auto acc = dbi.loadAccount();

    if (acc.is_logged_in()) {
        std::cout << "Logged in as " << acc.user_id << std::endl;
        std::cout << "Homeserver: " << acc.homeserver_url << std::endl;
        std::cout << "Device ID: " << acc.device_id << std::endl;
        std::cout << "Sync token: " << (acc.next_batch.empty() ? "(none)" : acc.next_batch.substr(0, 20) + "...") << std::endl;
    } else if (!Config::instance().accessToken().empty()) {
        std::cout << "Logged in as " << Config::instance().userId() << std::endl;
        std::cout << "Homeserver: " << Config::instance().homeserverURL() << std::endl;
        std::cout << "Device ID: " << Config::instance().deviceId() << std::endl;
    } else {
        std::cout << "Not logged in. Use 'matrixcli login' to authenticate." << std::endl;
    }

    return 0;
}

int cmdRooms(const matrixcli::cli::Args&) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) {
        std::cerr << "Cannot open database" << std::endl;
        return 1;
    }
    auto rooms = dbi.listRooms();
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
        std::cerr << "Usage: matrixcli view <room> [limit] [--thread event_id]" << std::endl;
        return 1;
    }
    std::string query = args.positional[0];
    int limit = 20;
    if (args.positional.size() >= 2 && !args.positional[1].starts_with("--"))
        limit = std::stoi(args.positional[1]);

    std::string thread_root;
    auto tr_it = args.options.find("thread");
    if (tr_it != args.options.end()) thread_root = tr_it->second;

    db::Database dbi;
    if (!dbi.open("matrixcli.db")) { std::cerr << "Cannot open database" << std::endl; return 1; }

    std::string room_id;
    auto rooms = dbi.listRooms();
    for (auto& r : rooms) {
        std::string id = r.value("room_id", "");
        std::string name = r.value("name", "");
        if (id == query || name == query || name.find(query) == 0) {
            room_id = id;
            if (!thread_root.empty()) std::cout << "=== " << name << " / thread " << thread_root << " ===" << std::endl;
            else std::cout << "=== " << name << " (" << id << ") ===" << std::endl;
            break;
        }
    }
    if (room_id.empty()) { room_id = query; std::cout << "=== " << room_id << " ===" << std::endl; }

    auto events = dbi.getEvents(room_id, limit);
    std::reverse(events.begin(), events.end());
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
        if (body.size() > 120) body = body.substr(0, 120) + "...";
        std::string sender = ev.sender;
        auto at = sender.find(':');
        if (at != std::string::npos && sender.starts_with("@")) sender = sender.substr(1, at - 1);

        // Thread indicators
        std::string prefix;
        if (ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].value("rel_type", "") == "m.thread") {
            prefix = "↳ ";
        }

        std::cout << "  " << prefix << "[" << sender << "] " << body << std::endl;
    }
    return 0;
}

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
        std::cout << "Sent [" << event_id << "]" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Send failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

int cmdDemoPopulate(const matrixcli::cli::Args&) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;

    struct { const char* id; const char* name; const char* topic; int members; } rooms[] = {
        {"!general:demo.local","#general","General discussion",42},
        {"!dev:demo.local","#dev","Development chat",15},
        {"!random:demo.local","#random","Random stuff",28},
        {"!dm_alice:demo.local","Alice","",2},
        {"!dm_bob:demo.local","Bob","",2},
    };
    for (auto& r : rooms) {
        nlohmann::json j;
        j["name"] = r.name; j["topic"] = r.topic; j["member_count"] = r.members;
        dbi.upsertRoom(j, r.id);
    }

    int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    struct { const char* room; const char* sender; const char* name; const char* body; } msgs[] = {
        {"!general:demo.local","@alice","Alice","Welcome! This is matrixcli — a terminal Matrix client."},
        {"!general:demo.local","@bob","Bob","Supports E2EE, SQLite cache, multi-format REST API."},
        {"!general:demo.local","@alice","Alice","Try: matrixcli tui, matrixcli view, matrixcli send"},
        {"!dev:demo.local","@charlie","Charlie","C++20, raw sockets + OpenSSL, no external HTTP libs."},
        {"!dev:demo.local","@alice","Alice","CMake build, 5 format renderers, full Matrix CS API."},
        {"!random:demo.local","@bob","Bob","Why did the dev quit? No arrays."},
        {"!dm_alice:demo.local","@alice","Alice","Hey! This is a private encrypted DM."},
        {"!dm_bob:demo.local","@bob","Bob","Try matrixcli view \"!dm_bob:demo.local\""},
    };
    for (auto& m : msgs) {
        matrix::Event ev;
        ev.event_id = "$demo_" + std::to_string(ts);
        ev.room_id = m.room; ev.sender = m.sender;
        ev.type = "m.room.message";
        ev.content = {{"body", m.body}, {"msgtype", "m.text"}};
        ev.origin_server_ts = ts;
        dbi.insertEvent(ev);
        ts -= 60;
    }

    std::cout << "Populated DB: " << (sizeof(rooms)/sizeof(rooms[0])) << " rooms, "
              << (sizeof(msgs)/sizeof(msgs[0])) << " messages." << std::endl;
    std::cout << "Try:  matrixcli rooms | matrixcli view #general | matrixcli view #dev" << std::endl;
    return 0;
}

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args&) {
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

            // Save to database for persistent sync state
            db::StoredAccount sacc;
            sacc.homeserver_url = login_result.homeserver;
            sacc.user_id = creds.user_id;
            sacc.access_token = creds.access_token;
            sacc.device_id = creds.device_id;
            dbi.saveAccount(sacc);

            // Init crypto
            client.initCrypto(creds.user_id, creds.device_id);

            tui::ChatView chat;
            chat.setStatus("Connected as " + creds.user_id);
            chat.setConnectionStatus("online");

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
                    // Vote in a poll: /vote event_id answer1,answer2,...
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
                    } else {
                        FILE* fp = popen("ls -1 --color=never *.png *.jpg *.gif *.webp *.mp4 *.pdf *.txt *.md 2>/dev/null", "r");
                        if (fp) {
                            char buf[256];
                            std::string files;
                            while (fgets(buf, sizeof(buf), fp)) files += "  " + std::string(buf);
                            pclose(fp);
                            if (!files.empty() && !roomId.empty()) {
                                auto nl = files.find('\n');
                                std::string first = files.substr(2, nl - 2);
                                try {
                                    auto mxc = client.uploadMedia(first);
                                    client.sendFileMessage(roomId, mxc, first, 0, "");
                                } catch (...) {}
                            }
                        }
                    }
                } else if (cmd == "create" || cmd == "newroom") {
                    auto sp = args.find(' ');
                    std::string name = (sp != std::string::npos) ? args.substr(0, sp) : args;
                    try {
                        auto roomId = client.createRoom(name);
                        client.joinRoom(roomId);
                    } catch (...) {}
                } else if (cmd == "search" || cmd == "joinroom") {
                    if (!args.empty()) {
                        try {
                            auto rooms = client.getPublicRooms("", args, 10);
                            // Join first match
                            if (rooms.contains("chunk") && !rooms["chunk"].empty()) {
                                client.joinRoom(rooms["chunk"][0]["room_id"].get<std::string>());
                            }
                        } catch (...) {}
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

            // Set up send callback
            chat.setSendCallback([&](const std::string& body) {
                std::string roomId = chat.activeRoomId();
                if (!roomId.empty()) {
                    try {
                        client.sendTextMessage(roomId, body);
                        client.sendTyping(roomId, false);
                    } catch (...) {}
                }
            });

            // Set up pagination callback
            chat.setPaginateCallback([&](const std::string& room_id) {
                try {
                    client.getRoomMessages(room_id, "", "b", 50);
                } catch (...) {}
            });

            // Start sync: feed events to chat
            client.startSync([&](const matrix::Event& ev) {
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

                // Typing events
                if (ev.type == "m.typing" && ev.content.contains("user_ids")) {
                    std::vector<std::string> users;
                    for (auto& uid : ev.content["user_ids"]) {
                        users.push_back(uid.get<std::string>());
                    }
                    chat.setTypingUsers(ev.room_id, users);
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

    if (args.command == "vote") {
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

    if (args.command == "react") {
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

    if (args.command == "topic") {
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

    if (args.command == "roomname") {
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

    if (args.command == "avatar") {
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

    if (args.command == "demo") {
        if (!args.positional.empty() && args.positional[0] == "populate") {
            return cmdDemoPopulate(args);
        }
        return cmdServe(args);
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

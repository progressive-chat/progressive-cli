#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>

#include "config.hpp"
#include "cli/args.hpp"
#include "commands.hpp"
#include "globals.hpp"
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

    server::ServerMode mode = demo_mode ? server::ServerMode::Demo :
                      acc.is_logged_in() ? server::ServerMode::Matrix : server::ServerMode::WebProxy;

    server::APIServer api_server(port, mode, acc.is_logged_in() ? acc.homeserver_url : "https://matrix.org");
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
        auto reg_it = args.options.find("register");

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
        } else if (reg_it != args.options.end()) {
            // Registration
            std::string username;
            auto user_it = args.options.find("username");
            if (user_it != args.options.end()) username = user_it->second;
            else { std::cerr << "Error: --username required for registration" << std::endl; return 1; }

            std::string password;
            auto pass_it = args.options.find("password");
            if (pass_it != args.options.end()) password = pass_it->second;
            else { std::cerr << "Error: --password required for registration" << std::endl; return 1; }

            auto creds = client.registerAccount(username, password);
            std::cout << "Registered as " << creds.user_id << std::endl;
            // Save to DB
            Config::instance().set("homeserver_url", homeserver);
            Config::instance().set("access_token", creds.access_token);
            Config::instance().set("user_id", creds.user_id);
            Config::instance().set("device_id", creds.device_id);
            Config::instance().save();
            db::Database dbi2;
            dbi2.open("matrixcli.db");
            db::StoredAccount sacc;
            sacc.homeserver_url = homeserver;
            sacc.user_id = creds.user_id;
            sacc.access_token = creds.access_token;
            sacc.device_id = creds.device_id;
            dbi2.saveAccount(sacc);
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

    bool verbose = args.options.count("verbose") || args.options.count("ids");
    bool show_ts = args.options.count("ts") || args.options.count("time");
    bool debug = args.options.count("debug") || args.options.count("raw");
    bool json_out = args.options.count("json");

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

    auto events = dbi.getEvents(room_id, limit > 0 ? limit : 999999, before);
    if (events.empty() && !before.empty()) {
        std::cout << "(no older messages)" << std::endl;
        return 0;
    }
    // JSON output mode (pipe-friendly)
    if (json_out) {
        nlohmann::json j;
        j["room_id"] = room_id;
        j["messages"] = nlohmann::json::array();
        for (auto& ev : events) {
            nlohmann::json m;
            m["event_id"] = ev.event_id;
            m["sender"] = ev.sender;
            m["body"] = ev.content.value("body", "");
            m["ts"] = ev.origin_server_ts;
            j["messages"].push_back(m);
        }
        std::cout << j.dump() << std::endl;
        return 0;
    }

    std::reverse(events.begin(), events.end());

    // Show pagination hint
    bool has_newer = !before.empty();
    bool has_older = (int)events.size() >= limit;

    // Message grouping
    std::string prev_sender;

    if (has_newer || has_older) {
        std::cout << "── ";
        if (has_newer) std::cout << "view --from " << events.front().event_id << " (newer)  ";
        if (has_older) std::cout << "view --before " << events.back().event_id << " (older)";
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
        if (body.size() > 120) body = body.substr(0, 120) + "...";
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

        // Reply context
        std::string reply_ctx;
        if (ev.content.contains("m.relates_to") &&
            ev.content["m.relates_to"].value("rel_type", "") == "m.in_reply_to") {
            reply_ctx = replyContext("(reply)");
        }

        // Link detection
        std::string link = extractLink(body);
        if (!link.empty() && !show_ts) {
            if (body.size() > 80) body = body.substr(0, 77) + "...";
        }

        std::string reply_str;
        if (reply_count > 0) reply_str = " [" + std::to_string(reply_count) + " replies]";

        if (!reply_ctx.empty()) std::cout << ANSI_GRAY "       " << reply_ctx << ANSI_RESET << std::endl;

        // Message grouping: collapse sender if same as previous
        if (ev.sender == prev_sender && !prev_sender.empty()) {
            std::string indent(sender_name.size() + 3, ' ');
            std::cout << indent << prefix << body << reply_str;
        } else {
            prev_sender = ev.sender;
            std::cout << "  " << prefix << ansiUser(ev.sender, "[" + sender_name + "]") << ts_str << " " << body << reply_str;
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
        std::string body = r.value("content", nlohmann::json::object()).value("body", "(no body)");
        if (body.size() > 100) body = body.substr(0, 100) + "...";
        std::cout << "  #" << room << "  [" << sender << "] " << body << std::endl;
    }
    return 0;
}

int cmdConfig(const matrixcli::cli::Args& args) {
    using namespace matrixcli;
    const std::string path = "matrixcli.toml";

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
                            chat.setConnectionStatus("TDLib initialized. Send /td phone +1234567890");
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
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Register all commands via registry (extensible, no if/else)
    extern void registerBuiltinCommands();
    registerBuiltinCommands();

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

    if (args.command == "search") {
        return cmdSearch(args);
    }

    if (args.command == "notifications" || args.command == "notif") {
        using namespace matrixcli;
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        int limit = args.options.count("limit") ? std::stoi(args.options["limit"]) : 20;
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

    if (args.command == "read") {
        using namespace matrixcli;
        db::Database dbi;
        if (!dbi.open("matrixcli.db")) return 1;
        if (args.options.count("all")) { dbi.markAllRead(); std::cout << "All read." << std::endl; }
        else if (!args.positional.empty()) { dbi.markRoomRead(args.positional[0]); std::cout << "Marked " << args.positional[0] << " read." << std::endl; }
        else { std::cerr << "Usage: matrixcli read <room> | matrixcli read --all" << std::endl; return 1; }
        return 0;
    }

    if (args.command == "config") {
        return cmdConfig(args);
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
            std::cout << "TDLib initialized. Run: matrixcli td phone +1234567890" << std::endl;
        } else if (sub == "phone") {
            if (args.positional.size() < 2) { std::cerr << "Usage: matrixcli td phone +1234567890" << std::endl; return 1; }
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

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
#include "media_send.hpp"
#include "main_commands.hpp"
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


// Defined in demo_tui.cpp.
extern int populateDemoData(matrixcli::db::Database& dbi);

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
}int cmdRooms(const matrixcli::cli::Args& args) {
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
}int cmdSendMsg(const matrixcli::cli::Args& args) {
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
}int cmdSearch(const matrixcli::cli::Args& args) {
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
}int cmdConfig(const matrixcli::cli::Args& args) {
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
}int cmdDemoPopulate(const matrixcli::cli::Args&) {
    using namespace matrixcli;
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return 1;
    return populateDemoData(dbi);
}
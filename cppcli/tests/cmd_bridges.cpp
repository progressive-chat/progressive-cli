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

int cmdTdBridge(const matrixcli::cli::Args& args) {

        // matrixcli td <subcommand> [args...]
        using namespace matrixcli;
        if (args.positional.empty()) {
            std::cerr << "Usage: progressive-cli td <login|phone|code|password|chats|msg|history>" << std::endl;
            return 1;
        }
        std::string sub = args.positional[0];

        if (sub == "login" || sub == "start") {
            if (!g_tdlib.isAvailable()) g_tdlib.initialize();
            if (!g_tdlib.isAvailable()) { std::cerr << "TDLib not available" << std::endl; return 1; }
            g_tdlib.setTdlibParams(94575, "a3406de8d171bb422bb6ddf3bbd8f4e2");
            std::cout << "TDLib initialized. Run: matrixcli td phone +123****7890" << std::endl;
        } else if (sub == "phone") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td phone +123****7890" << std::endl; return 1; }
            if (!g_tdlib.isAvailable() && !g_tdlib.initialize()) { std::cerr << "TDLib not available" << std::endl; return 1; }
            g_tdlib.sendPhoneNumber(args.positional[1]);
            std::cout << "Code sent. Run: matrixcli td code XXXXX" << std::endl;
        } else if (sub == "code") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td code XXXXX" << std::endl; return 1; }
            g_tdlib.sendAuthCode(args.positional[1]);
            std::cout << "Code sent. If 2FA: matrixcli td password yourpassword" << std::endl;
        } else if (sub == "password" || sub == "2fa") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td password your2fa" << std::endl; return 1; }
            g_tdlib.sendPassword(args.positional[1]);
            std::cout << "2FA sent. Run: matrixcli td chats" << std::endl;
        } else if (sub == "chats") {
            if (g_tdlib.authState() != tdlib::TdAuthState::Ready) { std::cerr << "Not authorized" << std::endl; return 1; }
            auto chats = g_tdlib.getChats(50);
            for (auto& c : chats) {
                std::cout << "  [" << c.id << "] " << c.title << " (" << c.type << ")" << " unread:" << c.unread_count << std::endl;
            }
        } else if (sub == "msg" || sub == "send") {
            if (args.positional.size() < 3) { std::cerr << "Usage: progressive-cli td msg <chat_id> <text>" << std::endl; return 1; }
            int64_t chatId = std::stoll(args.positional[1]);
            std::string text;
            for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) text += " "; text += args.positional[i]; }
            if (g_tdlib.authState() != tdlib::TdAuthState::Ready) { std::cerr << "Not authorized" << std::endl; return 1; }
            g_tdlib.sendMessage(chatId, text);
            std::cout << "Sent to chat " << chatId << std::endl;
        } else if (sub == "history" || sub == "view") {
            if (args.positional.size() < 2) { std::cerr << "Usage: progressive-cli td history <chat_id> [limit]" << std::endl; return 1; }
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
int cmdIrcBridge(const matrixcli::cli::Args& args) {

        using namespace matrixcli;
        if (args.positional.empty()) {
            std::cerr << "Usage: progressive-cli irc <connect|join|msg|leave|whois|names>" << std::endl;
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
int cmdLemmyBridge(const matrixcli::cli::Args& args) {

        using namespace matrixcli;
        if (args.positional.empty()) {
            std::cerr << "Usage: progressive-cli lemmy <login|posts|post|upvote|comments>" << std::endl;
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
int cmdDcBridge(const matrixcli::cli::Args& args) {

        using namespace matrixcli;
        if (args.positional.empty()) { std::cerr << "Usage: progressive-cli dc <login|chats|msg|history>" << std::endl; return 1; }
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

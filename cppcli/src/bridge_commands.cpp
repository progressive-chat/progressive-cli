#include "commands.hpp"
#include "globals.hpp"
#include "../lib/irc/irc_client.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace matrixcli;

static irc::IrcClient g_ircClient;
static bool g_ircSetup = false;

void registerBuiltinCommands() {
    auto& reg = CommandRegistry::instance();

    // ── IRC CLI commands ──
    reg.registerCli("irc", [](const cli::Args& args) -> int {
        if (args.positional.empty()) { std::cerr << "irc: connect|join|msg|leave|whois|names" << std::endl; return 1; }
        std::string sub = args.positional[0];
        if (sub == "connect") {
            irc::IrcServerConfig cfg;
            cfg.host = args.positional.size() > 1 ? args.positional[1] : "irc.libera.chat";
            cfg.port = args.positional.size() > 2 ? std::stoi(args.positional[2]) : 6667;
            cfg.nick = args.positional.size() > 3 ? args.positional[3] : "matrixcli";
            g_ircClient.setConfig(cfg);
            if (!g_ircSetup) {
                g_ircClient.onMessage([](const irc::IrcMessage& msg) {
                    std::cout << "[" << msg.target << "] <" << msg.prefix << "> " << msg.body << std::endl;
                });
                g_ircSetup = true;
            }
            g_ircClient.connect();
            std::this_thread::sleep_for(std::chrono::seconds(3));
        } else if (sub == "join" && args.positional.size() >= 2) g_ircClient.join(args.positional[1]);
        else if (sub == "msg" && args.positional.size() >= 3) {
            std::string t; for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) t += " "; t += args.positional[i]; }
            g_ircClient.privmsg(args.positional[1], t);
        } else if (sub == "leave" && args.positional.size() >= 2) g_ircClient.part(args.positional[1]);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return 0;
    });

    // ── Lemmy CLI commands ──
    reg.registerCli("lemmy", [](const cli::Args& args) -> int {
        if (args.positional.empty()) { std::cerr << "lemmy: login|posts|post|upvote|comments" << std::endl; return 1; }
        std::string sub = args.positional[0];
        if (sub == "login" && args.positional.size() >= 4) {
            g_lemmy.setInstance(args.positional[1]);
            if (g_lemmy.login(args.positional[2], args.positional[3])) std::cout << "OK" << std::endl;
            else { std::cerr << "Fail" << std::endl; return 1; }
        } else if (sub == "posts") {
            auto posts = g_lemmy.listPosts(args.positional.size() > 1 ? args.positional[1] : "", "Hot");
            for (auto& p : posts) std::cout << "[" << p.id << "] " << p.title << " ↑" << p.upvotes << " 💬" << p.comment_count << " " << p.community_name << std::endl;
        } else if (sub == "comments" && args.positional.size() >= 2) {
            for (auto& c : g_lemmy.listComments(std::stoi(args.positional[1])))
                std::cout << c.creator_name << ": " << c.content.substr(0, 100) << std::endl;
        } else if (sub == "post" && args.positional.size() >= 3) {
            std::string b; for (size_t i = 3; i < args.positional.size(); i++) { if (i > 3) b += " "; b += args.positional[i]; }
            std::cout << g_lemmy.createPost(args.positional[1], args.positional[2], b) << std::endl;
        } else if (sub == "upvote" && args.positional.size() >= 2) g_lemmy.likePost(std::stoi(args.positional[1]), 1);
        else if (sub == "downvote" && args.positional.size() >= 2) g_lemmy.likePost(std::stoi(args.positional[1]), -1);
        return 0;
    });

    // ── TDLib CLI commands ──
    reg.registerCli("td", [](const cli::Args& args) -> int {
        if (args.positional.empty()) { std::cerr << "td: login|phone|code|password|chats|msg|history" << std::endl; return 1; }
        std::string sub = args.positional[0];
        if (sub == "login") {
            if (!g_tdlib.isAvailable()) g_tdlib.initialize();
            if (g_tdlib.isAvailable()) g_tdlib.setTdlibParams(94575, "a3406de8d171bb422bb6ddf3bbd8f4e2");
        } else if (sub == "phone" && args.positional.size() >= 2) g_tdlib.sendPhoneNumber(args.positional[1]);
        else if (sub == "code" && args.positional.size() >= 2) g_tdlib.sendAuthCode(args.positional[1]);
        else if (sub == "chats") {
            auto chats = g_tdlib.getChats(20);
            for (auto& c : chats) std::cout << "[" << c.id << "] " << c.title << " (" << c.type << ")" << std::endl;
        } else if (sub == "msg" && args.positional.size() >= 3) {
            std::string t; for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) t += " "; t += args.positional[i]; }
            g_tdlib.sendMessage(std::stoll(args.positional[1]), t);
        } else if (sub == "history" && args.positional.size() >= 2) {
            for (auto& m : g_tdlib.getChatHistory(std::stoll(args.positional[1])))
                std::cout << (m.is_outgoing ? "→ " : "← ") << m.text.substr(0, 100) << std::endl;
        }
        return 0;
    });

    // ── DeltaChat CLI commands ──
    reg.registerCli("dc", [](const cli::Args& args) -> int {
        if (args.positional.empty()) { std::cerr << "dc: login|chats|msg|history" << std::endl; return 1; }
        std::string sub = args.positional[0];
        if (sub == "login") {
            if (!g_dc.isAvailable()) g_dc.initialize();
            if (args.positional.size() >= 3) { g_dc.setConfig("addr", args.positional[1]); g_dc.setConfig("mail_pw", args.positional[2]); }
            g_dc.configure();
        } else if (sub == "chats") {
            for (auto& c : g_dc.getChatList()) std::cout << "[" << c.id << "] " << c.name << std::endl;
        } else if (sub == "msg" && args.positional.size() >= 3) {
            std::string t; for (size_t i = 2; i < args.positional.size(); i++) { if (i > 2) t += " "; t += args.positional[i]; }
            g_dc.sendMessage(std::stoi(args.positional[1]), t);
        } else if (sub == "history" && args.positional.size() >= 2) {
            for (auto& m : g_dc.getChatMessages(std::stoi(args.positional[1])))
                std::cout << (m.is_outgoing ? "→ " : "← ") << m.sender_name << ": " << m.text.substr(0, 100) << std::endl;
        }
        return 0;
    });

    // ── Version ──
    reg.registerCli("version", [](const cli::Args&) -> int {
        std::cout << "matrixcli v0.1.0" << std::endl;
        return 0;
    });
}

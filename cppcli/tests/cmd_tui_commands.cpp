// src/demo_tui.cpp — the demo data, the demo REPL and the ncurses TUI
// (split out of main.cpp so the compilation stays incremental-friendly).
#include "commands.hpp"
#include "config.hpp"
#include "cli/args.hpp"
#include "core/http_client.hpp"
#include "globals.hpp"
#include "pcore.hpp"
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"
#include "../lib/util/string_utils.hpp"
#include "../lib/util/client_utils.hpp"
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/agent_setup.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace matrixcli;

#include "demo_tui.hpp"

extern int runSasVerification(const std::string&, const std::string&,
                              int, bool,
                              const std::function<void(const std::string&)>&,
                              const std::function<bool()>&);

void queueUrlPreview(matrixcli::matrix::Client& client, matrixcli::tui::ChatView& chat,
                            const std::string& roomId, const std::string& body) {
    static std::unordered_set<std::string> fetched;
    static std::mutex fetchedMtx;
    const auto httpPos = body.find("http");
    if (httpPos == std::string::npos) return;
    const auto endPos = body.find_first_of(" \t\n", httpPos);
    const std::string url = body.substr(httpPos, endPos == std::string::npos
                                                     ? std::string::npos
                                                     : endPos - httpPos);
    if (url.size() < 10) return;
    {
        std::lock_guard<std::mutex> lk(fetchedMtx);
        if (!fetched.insert(url).second) return;
    }
    std::thread([&client, &chat, roomId, url]() {
        try {
            const nlohmann::json preview = client.getURLPreview(url);
            const std::string title = preview.value("og:title", "");
            if (title.empty()) return;
            matrixcli::tui::MessageInfo mi;
            mi.sender = "@preview";
            mi.is_notice = true;
            mi.body = "\U0001F517 " + title;
            chat.addMessage(roomId, mi);
        } catch (...) {}
    }).detach();
}

void tuiHandleCommand(matrixcli::tui::ChatView& chat,
                      matrixcli::tui::Screen& screen,
                      matrixcli::matrix::Client& client,
                      matrixcli::db::Database& dbi,
                      const matrixcli::tui::TUIConfig& tuiCfg,
                      std::atomic<bool>& sasConfirm,
                      std::atomic<bool>& sasCancel,
                      const std::function<void(const std::string&)>& launchMatrixAgent,
                      const std::string& cmd, const std::string& args) {
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
                    if (!roomId.empty() && !args.empty()) {
                        // The moderation guard: the others' messages are
                        // protected unless modredact is on.
                        matrix::Event target;
                        if (dbi.getEventById(args, target) &&
                            target.sender != client.userId() &&
                            dbi.getSetting("mod_redact", "0") == "0") {
                            chat.setConnectionStatus("redaction blocked (modredact off)");
                        } else {
                            client.redactEvent(roomId, args);
                        }
                    }
                } else if (cmd == "edit") {
                    std::string roomId = chat.activeRoomId();
                    auto sp = args.find(' ');
                    if (!roomId.empty() && sp != std::string::npos) {
                        nlohmann::json c = {{"msgtype","m.text"},{"body","* "+args.substr(sp+1)},
                            {"m.new_content",{{"msgtype","m.text"},{"body",args.substr(sp+1)}}},
                            {"m.relates_to",{{"event_id",args.substr(0,sp)},{"rel_type","m.replace"}}}};
                        try { client.sendEvent(roomId, "m.room.message", c); } catch (...) {}
                    }
                } else if (cmd == "knock") {
                    if (!args.empty()) try { client.knockRoom(args); } catch (...) {}
                } else if (cmd == "read" || cmd == "markread") {
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        // The local last-read marker moves to the newest
                        // cached event (the m.fully_read copy).
                        auto evs = dbi.getEvents(roomId, 1);
                        if (!evs.empty()) dbi.setReadMarker(roomId, evs.front().event_id);
                        // The server-side marker follows the per-room
                        // receipts policy (see /receipts).
                        if (dbi.receiptsEnabled(roomId)) {
                            try { client.sendReadReceipt(roomId, ""); } catch (...) {}
                            chat.setConnectionStatus("marked read (receipt sent)");
                        } else {
                            chat.setConnectionStatus("marked read locally (receipts off)");
                        }
                    }
                } else if (cmd == "receipts") {
                    // /receipts on|off — the read-receipt policy for the
                    // active room.
                    std::string roomId = chat.activeRoomId();
                    if (!roomId.empty()) {
                        if (args == "off") {
                            dbi.setReceiptsEnabled(roomId, false);
                            chat.setConnectionStatus("receipts off for this room");
                        } else if (args == "on") {
                            dbi.setReceiptsEnabled(roomId, true);
                            chat.setConnectionStatus("receipts on for this room");
                        } else {
                            chat.setConnectionStatus(std::string("receipts: ")
                                + (dbi.receiptsEnabled(roomId) ? "on" : "off")
                                + " — /receipts on|off");
                        }
                    }
                } else if (cmd == "verify") {
                    // /verify <@user:server> <deviceId> — the SAS device
                    // verification in the chat: the emojis land here, the
                    // match is confirmed with /verify-confirm (cancelled
                    // with /verify-cancel). Uses the ecore session (run
                    // 'progressive-cli login' once for the crypto identity).
                    std::istringstream ss(args);
                    std::string user, device;
                    ss >> user >> device;
                    if (user.empty() || device.empty()) {
                        chat.setConnectionStatus("usage: /verify <@user:server> <deviceId>");
                    } else {
                        sasConfirm = false;
                        sasCancel = false;
                        std::string roomId = chat.activeRoomId();
                        if (roomId.empty()) roomId = "!agent:demo.local";
                        chat.setConnectionStatus("verifying " + user + "/"
                                                 + device + " — /verify-confirm | /verify-cancel");
                        std::thread([&, user, device, roomId]() {
                            auto post = [&](const std::string& s) {
                                tui::MessageInfo mi;
                                mi.sender = "@verify";
                                mi.is_notice = true;
                                mi.body = s;
                                chat.addMessage(roomId, mi);
                            };
                            const int rc = runSasVerification(
                                user, device, 180, false, post,
                                [&]() -> bool {
                                    // Wait for the user's /verify-confirm
                                    // (or the cancel).
                                    for (int i = 0; i < 1200; i++) {
                                        if (sasCancel.load()) return false;
                                        if (sasConfirm.load()) return true;
                                        usleep(100000);
                                    }
                                    return false;
                                });
                            post(rc == 0 ? "\u2713 verification done"
                                         : "verification failed or cancelled");
                            chat.setConnectionStatus("verification finished");
                        }).detach();
                    }
                } else if (cmd == "verify-confirm") {
                    sasConfirm = true;
                    chat.setConnectionStatus("confirmation accepted — sending the MAC");
                } else if (cmd == "verify-cancel") {
                    sasCancel = true;
                    chat.setConnectionStatus("cancellation requested");
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
                } else if (cmd == "agent") {
                    // The Matrix agent (Android-parity /agent): the LLM
                    // loop with the Matrix tools (read/search/send messages
                    // etc.) runs in a background thread; the progress + the
                    // answer land in the chat as @agent messages. Esc
                    // interrupts the run.
                    launchMatrixAgent(args);
                } else if (cmd == "agent-code") {
                    // The coding agent in the TUI: the run in a background
                    // thread, the progress + the answer land in the chat.
                    std::string prompt = args;
                    std::string roomId = chat.activeRoomId();
                    if (roomId.empty()) roomId = "!agent:demo.local";
                    static std::atomic<bool> agentCodeBusy{false};
                    if (agentCodeBusy.exchange(true)) {
                        chat.setConnectionStatus("agent-code busy — wait or Esc");
                        agentCodeBusy = false;
                    } else {
                        matrixcli::g_agentInterrupt = false;
                        chat.setConnectionStatus("agent-code running: "
                                                 + prompt.substr(0, 30) + "...");
                        std::thread([&, prompt, roomId]() {
                            agenttools::Config cfg;
                            agenttools::loadAgentConfig(cfg);
                            if (cfg.key.empty()) {
                                const char* env = cfg.provider == "anthropic"
                                    ? std::getenv("ANTHROPIC_API_KEY")
                                    : std::getenv("OPENAI_API_KEY");
                                if (env && *env) cfg.key = env;
                            }
                            std::vector<agenttools::Message> hist;
                            agenttools::Result res = agenttools::run(
                                cfg, prompt, hist, nullptr, nullptr,
                                [&](const std::string& l) {
                                    tui::MessageInfo mi;
                                    mi.sender = "@agent-code";
                                    mi.body = l;
                                    chat.addMessage(roomId, mi);
                                },
                                nullptr);
                            tui::MessageInfo mi;
                            mi.sender = "@agent-code";
                            mi.body = res.ok ? res.text
                                             : "[agent error] " + res.error;
                            chat.addMessage(roomId, mi);
                            chat.setConnectionStatus("agent-code done");
                            agentCodeBusy = false;
                        }).detach();
                    }
                } else if (cmd == "llm") {
                    // A single LLM completion (the Android /llm parity).
                    std::string prompt = args;
                    std::string roomId = chat.activeRoomId();
                    if (roomId.empty()) roomId = "!agent:demo.local";
                    std::thread([&, prompt, roomId]() {
                        matrixcli::matrixagent::Config cfg;
                        matrixcli::matrixagent::applyDefaults(cfg);
                        auto cres = matrixcli::matrixagent::completeEx(
                            cfg, "", prompt);
                        tui::MessageInfo mi;
                        mi.sender = "@llm";
                        mi.body = cres
                            ? (cres->text.empty() ? "(empty response)" : cres->text)
                            : "[llm error] " + cres.error();
                        chat.addMessage(roomId, mi);
                    }).detach();
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
                            chat.setConnectionStatus("TDLib initialized. Send /td phone +123****7890");
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
}

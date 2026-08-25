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

extern void persistActiveProxy(const progressive::desktop::ProxyConfig& cfg);
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

static void applyConnectionChoice(matrixcli::matrix::Client& client,
                                  const std::string& connection) {
    const std::string c = connection.empty() ? "direct" : connection;
    matrixcli::http::ProxyConfig pc;
    bool enabled = false;
    if (c == "tor") {
        pc.type = matrixcli::http::ProxyType::SOCKS5;
        pc.host = "127.0.0.1";
        pc.port = 9050;
        enabled = true;
    } else if (c == "i2p") {
        pc.type = matrixcli::http::ProxyType::HTTP;
        pc.host = "127.0.0.1";
        pc.port = 4444;
        enabled = true;
    } else if (c.rfind("custom ", 0) == 0) {
        pc.type = matrixcli::http::ProxyType::SOCKS5;
        const std::string hp = c.substr(7);
        const auto colon = hp.rfind(':');
        pc.host = colon == std::string::npos ? hp : hp.substr(0, colon);
        if (colon != std::string::npos) {
            try { pc.port = std::stoi(hp.substr(colon + 1)); }
            catch (...) { pc.port = 9050; }
        } else {
            pc.port = 9050;
        }
        enabled = true;
    }
    // "direct" and "yggdrasil" (the native IPv6 mesh routing) stay direct.

    client.setProxy(enabled ? pc : matrixcli::http::ProxyConfig{});

    progressive::desktop::ProxyConfig gp;
    gp.enabled = enabled;
    gp.host = pc.host;
    gp.port = pc.port;
    gp.type = pc.type == matrixcli::http::ProxyType::HTTP
                  ? progressive::desktop::ProxyConfig::Type::Http
                  : progressive::desktop::ProxyConfig::Type::Socks5Hostname;
    progressive::desktop::setGlobalProxy(gp);
    persistActiveProxy(gp);
}

#include "demo_tui.hpp"

extern int runSasVerification(const std::string&, const std::string&,
                              int, bool,
                              const std::function<void(const std::string&)>&,
                              const std::function<bool()>&);

#ifdef BUILD_TUI
int cmdTUI(const matrixcli::cli::Args& args) {
    using namespace matrixcli;

    Config::instance().load("config.json");
    matrix::Client client;

    db::Database dbi;
    dbi.open("matrixcli-demo.db");
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

    // Offline agent mode: `progressive-cli tui agent [task]` — no Matrix login
    // needed. The TUI opens straight into the chat with a virtual #agent
    // room; /agent, /agent-code and /llm work (the live Matrix tools
    // answer "no matrix session", the cache-backed ones work offline).
    bool agentOnly = false;
    std::string agentTask;
    if (!args.positional.empty() && args.positional[0] == "agent") {
        agentOnly = true;
        for (size_t i = 1; i < args.positional.size(); i++) {
            if (i > 1) agentTask += " ";
            agentTask += args.positional[i];
        }
    }
    if (args.options.count("agent")) agentOnly = true;

    tui::Screen screen;
    screen.init();

    // Demo TUI: skip the login view — the demo database is already
    // populated, the chat opens straight away (offline).
    tui::LoginResult login_result;
    if (agentOnly) {
        login_result.success = true;
        login_result.username = "offline";
        login_result.homeserver = "";
        login_result.password = "";
    } else if (args.options.count("tui")) {
        db::Database checkDb;
        if (checkDb.open("matrixcli-demo.db") && checkDb.listRooms().empty()) {
            populateDemoData(checkDb);
        }
        login_result.success = true;
        login_result.username = "demo";
        login_result.homeserver = "demo.local";
        login_result.password = "";
    } else {
        tui::LoginView login_view;
        login_result = login_view.run(screen);
    }

    if (login_result.success) {
        try {
            bool demoMode = args.options.count("tui");
            db::StoredAccount sacc;
            if (agentOnly) {
                // No login, and the stored account is left untouched —
                // the agent mode must not overwrite the real login.
                sacc.homeserver_url = "";
                sacc.user_id = "@offline:localhost";
                sacc.access_token = "";
                sacc.device_id = "";
            } else {
                client.setHomeserverURL(login_result.homeserver);
                if (demoMode) {
                    // Offline demo: a fake user, no network login.
                    sacc.homeserver_url = "demo.local";
                    sacc.user_id = "@demo:demo.local";
                    sacc.access_token = "";
                    sacc.device_id = "demo-tui";
                } else {
                    auto creds = client.loginPassword(login_result.username, login_result.password);
                    // The login-screen connection choice (direct/tor/i2p/
                    // yggdrasil/custom): applies to the TUI client now and
                    // persists into config.json for the CLI + the global
                    // core proxy.
                    applyConnectionChoice(client, login_result.connection);
                    Config::instance().set("homeserver_url", login_result.homeserver);
                    Config::instance().set("access_token", creds.access_token);
                    Config::instance().set("user_id", creds.user_id);
                    Config::instance().set("device_id", creds.device_id);
                    Config::instance().save();
                    sacc.homeserver_url = login_result.homeserver;
                    sacc.user_id = creds.user_id;
                    sacc.access_token = creds.access_token;
                    sacc.device_id = creds.device_id;
                    // Init crypto
                    client.initCrypto(creds.user_id, creds.device_id);
                }
            }
            if (!agentOnly) dbi.saveAccount(sacc);

            // First-run agent setup: no configured API key → the wizard
            // asks for the provider preset, the key and the extras, then
            // persists everything into ~/.config/matrixcli/agent.json.
            if (agentOnly) {
                matrixcli::matrixagent::Config probe;
                matrixcli::matrixagent::applyDefaults(probe);
                if (probe.key.empty()) {
                    tui::AgentSetupView setup;
                    tui::AgentSetupResult sr = setup.run(screen);
                    if (sr.ok) {
                        agenttools::Config out;
                        if (!sr.provider.empty() && sr.provider != "custom") {
                            agenttools::applyProviderPreset(out, sr.provider);
                        } else {
                            out.provider = "openai";
                        }
                        if (!sr.endpoint.empty()) out.endpoint = sr.endpoint;
                        if (!sr.model.empty()) out.model = sr.model;
                        out.key = sr.key;
                        out.proxy = sr.proxy;
                        out.trust = "ask";
                        agenttools::saveAgentConfig(out);
                    }
                }
            }

            tui::ChatView chat;
            {
                char cwdBuf[4096];
                std::string cwd = getcwd(cwdBuf, sizeof(cwdBuf))
                                      ? cwdBuf : std::string(".");
                chat.setStatus(agentOnly
                                   ? "offline agent mode · /agent /agent-code /llm · " + cwd
                                   : "Connected as " + sacc.user_id + " · " + cwd);
            }
            chat.setConnectionStatus(agentOnly ? "offline (agent mode)"
                                               : demoMode ? "demo (offline)"
                                                          : "online");

            // Load TUI config
            tui::TUIConfig tuiCfg = tui::TUIConfig::load("matrixcli.toml");
            if (args.options.count("no-mouse")) tuiCfg.mouse_enabled = false;
            if (args.options.count("mouse")) tuiCfg.mouse_enabled = true;
            screen.setMouseEnabled(tuiCfg.mouse_enabled);

            // Command handler for slash commands
            chat.setEscHandler([]() { matrixcli::g_agentInterrupt = true; });
            // Ctrl+C exits the TUI (the same SIGINT flag the agent loops
            // and the REPLs observe).
            chat.setQuitCheck([]() { return !matrixcli::g_interrupted.load(); });

            // The Matrix agent launcher — shared by the /agent slash
            // command and the `progressive-cli tui agent <task>` auto-start.
            static std::atomic<bool> agentBusy{false};
            auto launchMatrixAgent = [&](const std::string& task) {
                if (task.empty()) return;
                std::string roomId = chat.activeRoomId();
                if (roomId.empty()) roomId = "!agent:demo.local";
                if (agentBusy.exchange(true)) {
                    chat.setConnectionStatus("agent busy — wait or Esc");
                    agentBusy = false;
                    return;
                }
                matrixcli::g_agentInterrupt = false;
                chat.setConnectionStatus("agent running: "
                                         + task.substr(0, 30) + "...");
                std::thread([&, task, roomId]() {
                    matrixcli::matrixagent::Config cfg;
                    cfg.verbose = true;
                    matrixcli::matrixagent::applyDefaults(cfg);
                    auto backend = matrixcli::matrixagent::makeMatrixBackend(&client);
                    matrixcli::matrixagent::Result res = matrixcli::matrixagent::run(
                        cfg, *backend, task, roomId,
                        [&](const std::string& l) {
                            tui::MessageInfo mi;
                            mi.sender = "@agent";
                            mi.body = l;
                            chat.addMessage(roomId, mi);
                        });
                    tui::MessageInfo mi;
                    mi.sender = "@agent";
                    mi.body = res.ok
                        ? (res.answer.empty() ? "[agent] done, no final answer"
                                              : res.answer)
                        : "[agent error] " + res.error;
                    chat.addMessage(roomId, mi);
                    chat.setConnectionStatus("agent done");
                    agentBusy = false;
                }).detach();
            };

            // The SAS confirmation flags (shared by /verify, /verify-confirm
            // and /verify-cancel).
            std::atomic<bool> sasConfirm{false};
            std::atomic<bool> sasCancel{false};
            chat.setCommandHandler([&](const std::string& cmd, const std::string& args) {
                tuiHandleCommand(chat, screen, client, dbi, tuiCfg,
                                 sasConfirm, sasCancel, launchMatrixAgent,
                                 cmd, args);
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
            std::map<std::string, db::InviteInfo> inviteMap;
            if (!agentOnly) {
                // Mark the invited rooms with the ✉ marker; the remembered
                // invitation date shows as a line in the room's chat below.
                std::string selfId = demoMode ? "@you:demo.local" : sacc.user_id;
                for (auto& inv : dbi.openInvites(selfId)) inviteMap[inv.roomId] = inv;
                std::set<std::string> haveIds;
                for (auto& ri : roomInfos) haveIds.insert(ri.id);
                for (auto& ri : roomInfos) {
                    if (inviteMap.count(ri.id)) ri.name = "✉ " + ri.name;
                }
                for (auto& [id, inv] : inviteMap) {
                    if (haveIds.count(id)) continue;
                    tui::RoomInfo ri;
                    ri.id = id;
                    ri.name = "✉ " + id;
                    roomInfos.push_back(ri);
                }
            }
            if (roomInfos.empty()) {
                // Add a placeholder
                tui::RoomInfo ri;
                ri.id = "!welcome:demo.local";
                ri.name = "#welcome";
                roomInfos.push_back(ri);
            }
            if (agentOnly) {
                // The virtual #agent room: the home for the agent output
                // when there is no Matrix session.
                tui::RoomInfo ri;
                ri.id = "!agent:demo.local";
                ri.name = "#agent";
                roomInfos.insert(roomInfos.begin(), ri);
            }
            chat.setRooms(roomInfos);

            // Load the cached history from the local DB into the chat:
            // the demo TUI works offline (the chat was stuck on
            // "(no messages)" forever), and the real client shows its
            // history instantly instead of waiting for the first sync.
            for (auto& ri : roomInfos) {
                auto evs = dbi.getEvents(ri.id, 100);
                auto invIt = inviteMap.find(ri.id);
                if (evs.empty() && invIt == inviteMap.end()) continue;
                std::vector<tui::MessageInfo> msgs;
                if (invIt != inviteMap.end()) {
                    // The remembered invitation date: a line at the top of
                    // the room's chat ("invited you 2h ago (from alice) —
                    // "reason"").
                    tui::MessageInfo imi;
                    imi.sender = "@invite";
                    imi.is_notice = true;
                    std::string who = invIt->second.inviter;
                    auto at2 = who.find(':');
                    if (at2 != std::string::npos) who = who.substr(1, at2 - 1);
                    else if (!who.empty() && who[0] == '@') who = who.substr(1);
                    imi.body = "invited you " + relativeTime(invIt->second.ts)
                             + " (from " + who + ")";
                    if (!invIt->second.reason.empty()) {
                        imi.body += " — \"" + invIt->second.reason + "\"";
                    }
                    msgs.push_back(imi);
                }
                for (auto& ev : evs) {
                    if (ev.type != "m.room.message" && ev.type != "m.sticker") continue;
                    tui::MessageInfo mi;
                    std::string s = ev.sender;
                    auto at = s.find(':');
                    if (at != std::string::npos) s = s.substr(1, at - 1);
                    else if (!s.empty() && s[0] == '@') s = s.substr(1);
                    mi.sender = s;
                    mi.body = ev.content.value("body", "(no body)");
                    mi.event_id = ev.event_id;
                    mi.is_notice = ev.content.value("msgtype", "") == "m.notice";
                    mi.is_encrypted = ev.content.value("msgtype", "") == "m.encrypted";
                    msgs.push_back(mi);
                }
                if (!msgs.empty()) chat.setMessages(ri.id, msgs);
                // The last-read divider: a dim notice right after the
                // message the user read up to (the local m.fully_read copy).
                std::string marker = dbi.getReadMarker(ri.id);
                if (!marker.empty()) {
                    auto it = std::find_if(msgs.begin(), msgs.end(),
                        [&](const tui::MessageInfo& m) { return m.event_id == marker; });
                    if (it != msgs.end()) {
                        tui::MessageInfo div;
                        div.is_notice = true;
                        div.body = "── last read ──";
                        if (it + 1 != msgs.end()) msgs.insert(it + 1, div);
                        else msgs.push_back(div);
                        chat.setMessages(ri.id, msgs);
                    }
                }
            }

            // Ctrl+F search: filter the active room's cached messages to
            // the matches (the Go-TUI parity).
            chat.setSearchCallback([&](const std::string& query) {
                std::string roomId = chat.activeRoomId();
                if (roomId.empty() || query.empty()) {
                    chat.setConnectionStatus("search: nothing to search");
                    return;
                }
                auto rows = dbi.search(query, 50);
                std::vector<tui::MessageInfo> msgs;
                for (auto& r : rows) {
                    if (r.value("room_id", "") != roomId) continue;
                    tui::MessageInfo mi;
                    std::string s = r.value("sender", "");
                    auto at = s.find(':');
                    if (at != std::string::npos) s = s.substr(1, at - 1);
                    else if (!s.empty() && s[0] == '@') s = s.substr(1);
                    mi.sender = s;
                    mi.body = r.value("content", nlohmann::json::object())
                                  .value("body", "");
                    mi.event_id = r.value("event_id", "");
                    msgs.push_back(mi);
                }
                if (!msgs.empty()) {
                    tui::MessageInfo notice;
                    notice.sender = "@search";
                    notice.is_notice = true;
                    notice.body = std::to_string(msgs.size())
                                + " results for \"" + query
                                + "\" (Esc/Ctrl+F to reset)";
                    msgs.insert(msgs.begin(), notice);
                }
                chat.setMessages(roomId, msgs);
                chat.setConnectionStatus("search: " + std::to_string(msgs.size())
                                         + " results for \"" + query + "\"");
            });

            // The typing notifications: the composer fires the hook on the
            // printable keys; the throttle + the send_typing setting gate
            // the actual sends.
            static std::chrono::steady_clock::time_point lastTypingSent{};
            chat.setTypeNotify([&]() {
                if (dbi.getSetting("send_typing", "1") == "0") return;
                const auto now = std::chrono::steady_clock::now();
                if (now - lastTypingSent < std::chrono::seconds(10)) return;
                lastTypingSent = now;
                std::string roomId = chat.activeRoomId();
                if (!roomId.empty()) {
                    try { client.sendTyping(roomId, true, 20000); } catch (...) {}
                }
            });

            // Set up send callback with retry queue
            chat.setSendCallback([&](const std::string& body) {
                std::string roomId = chat.activeRoomId();
                if (!roomId.empty()) {
                    if (agentOnly) {
                        // Offline: the message stays local (and unsent).
                        tui::MessageInfo mi;
                        mi.sender = "@you";
                        mi.body = body;
                        chat.addMessage(roomId, mi);
                        chat.setConnectionStatus("offline agent mode — the message was not sent");
                        return;
                    }
                    try {
                        client.sendTextMessage(roomId, body);
                        if (dbi.getSetting("send_typing", "1") != "0")
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
            // (skipped in the offline agent mode — there is no session).
            if (!agentOnly) {
            client.startSync([&](const matrix::Event& ev) {
                tuiHandleEvent(chat, screen, client, dbi, tuiCfg, ev);
            });

            }  // !agentOnly

            // Offline agent mode with a task: fire the agent once on
            // startup (`progressive-cli tui agent <task>`).
            if (agentOnly && !agentTask.empty()) launchMatrixAgent(agentTask);

            chat.run(screen);
            if (!agentOnly) client.stopSync();
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

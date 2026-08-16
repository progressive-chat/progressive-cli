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

void tuiHandleEvent(matrixcli::tui::ChatView& chat,
                    matrixcli::tui::Screen& screen,
                    matrixcli::matrix::Client& client,
                    matrixcli::db::Database& dbi,
                    const matrixcli::tui::TUIConfig& tuiCfg,
                    const matrix::Event& ev) {
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
                    // The async link preview (the first URL in the body).
                    queueUrlPreview(client, chat, ev.room_id, mi.body);
                }

                // The read receipts: how many users read up to each event.
                if (ev.type == "m.receipt" && ev.content.is_object()) {
                    for (auto& [eventId, receipts] : ev.content.items()) {
                        if (!receipts.is_object()) continue;
                        auto mread = receipts.value("m.read", nlohmann::json::object());
                        if (!mread.is_object()) continue;
                        int count = 0;
                        for (auto& [userId, ts] : mread.items()) (void)ts, count++;
                        if (count > 0) chat.setReceipts(ev.room_id, eventId, count);
                    }
                }
                // Redactions
                if (ev.type == "m.room.redaction" && !ev.redacts.empty()) {                    tui::MessageInfo mi;
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
}

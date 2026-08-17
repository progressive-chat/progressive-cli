// src/ascii_settings.cpp — the settings + the display commands (split out
// of ascii_ui.cpp so the compilation stays incremental-friendly).
#include "ascii_state.hpp"

#include "../lib/database/db.hpp"
#include "pcore.hpp"
#include "../lib/matrix/client.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

using namespace matrixcli;

namespace matrixcli {

bool asciiSettingsCommand(UiState& st, db::Database& dbi, const cli::Args& a) {
        if (a.command == "names") {
            if (a.positional.empty() || a.positional[0] == "on") st.showNames = true;
            else st.showNames = false;
            dbi.setSetting("names", st.showNames ? "1" : "0");
            st.statusNote = std::string("sender names ") + (st.showNames ? "shown" : "hidden");
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- receipts: the display + the per-room SEND policy ----
        if (a.command == "receipts") {
            if (a.positional.empty()) {
                std::cout << "receipts show: " << (st.showReceipts ? "on" : "off")
                          << " | send (this room): "
                          << (dbi.receiptsEnabled(st.currentRoomId) ? "on" : "off")
                          << std::endl
                          << "  receipts show on|off — whether to SHOW them\n"
                          << "  receipts send on|off — whether to SEND them (this room)\n"
                          << "  receipts on|off       — the display toggle (the Element habit)"
                          << std::endl;
                return true;
            }
            if (a.positional[0] == "send") {
                const bool on = a.positional.size() < 2 || a.positional[1] != "off";
                dbi.setReceiptsEnabled(st.currentRoomId, on);
                st.statusNote = std::string("read receipts SENT: ") + (on ? "on" : "off")
                              + " for " + st.currentRoomId;
                std::cout << drawFrame(st) << std::flush;
                return true;
            }
            if (a.positional[0] == "show") {
                st.showReceipts = a.positional.size() < 2 || a.positional[1] != "off";
                dbi.setSetting("receipts", st.showReceipts ? "1" : "0");
                st.statusNote = std::string("read receipts ") + (st.showReceipts ? "shown" : "hidden");
                std::cout << drawFrame(st) << std::flush;
                return true;
            }
            if (a.positional[0] == "on") st.showReceipts = true;
            else if (a.positional[0] == "off") st.showReceipts = false;
            else if (a.positional[0] == "current") {
                st.showReceipts = true;
                dbi.setSetting("receipts_mode", "current");
                st.statusNote = "read receipts: only the current marker";
                std::cout << drawFrame(st) << std::flush;
                return true;
            } else if (a.positional[0] == "all") {
                st.showReceipts = true;
                dbi.setSetting("receipts_mode", "all");
                st.statusNote = "read receipts: every received receipt";
                std::cout << drawFrame(st) << std::flush;
                return true;
            } else {
                st.showReceipts = false;
            }
            dbi.setSetting("receipts", st.showReceipts ? "1" : "0");
            st.statusNote = std::string("read receipts ") + (st.showReceipts ? "shown" : "hidden");
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- sendtyping on|off: whether the client SENDS the typing
        // notifications (the TUI composer) ----
        if (a.command == "sendtyping") {
            const bool on = a.positional.empty() || a.positional[0] != "off";
            dbi.setSetting("send_typing", on ? "1" : "0");
            st.statusNote = std::string("typing notifications SENT: ") + (on ? "on" : "off");
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- joins on|off: Element "show join/leave messages" ----
        if (a.command == "joins") {
            if (a.positional.empty() || a.positional[0] == "on") st.showJoins = true;
            else st.showJoins = false;
            dbi.setSetting("joins", st.showJoins ? "1" : "0");
            st.statusNote = std::string("join/leave rows ") + (st.showJoins ? "shown" : "hidden");
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- links on|off: Element "enable URL previews" ----
        if (a.command == "links" && a.positional.size() >= 1 &&
            (a.positional[0] == "on" || a.positional[0] == "off")) {
            st.showLinks = (a.positional[0] == "on");
            dbi.setSetting("links", st.showLinks ? "1" : "0");
            st.statusNote = std::string("link pills ") + (st.showLinks ? "shown" : "raw URLs");
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- clock 12h|24h: Element "24-hour clock" ----
        if (a.command == "clock") {
            std::string v = a.positional.empty() ? "12h" : a.positional[0];
            st.clock12h = (v == "12h" || v == "12" || v == "am");
            dbi.setSetting("clock12h", st.clock12h ? "1" : "0");
            st.statusNote = std::string("clock ") + (st.clock12h ? "12h (AM/PM)" : "24h");
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- poll <event_id>: the poll variants and the voters ----
        if (a.command == "poll") {
            if (a.positional.empty()) {
                std::cout << "Usage: poll <event_id>   (ids on shows the ids)"
                          << std::endl;
                return true;
            }
            matrix::Event pev;
            if (!st.db->getEventById(a.positional[0], pev) ||
                pev.content.value("msgtype", "") != "m.poll.start") {
                std::cout << "No poll with that id." << std::endl;
                return true;
            }
            std::string qtext;
            auto q = pev.content.find("question");
            if (q != pev.content.end() && q->is_object()) {
                qtext = q->value("text", "");
            }
            std::cout << "Poll: " << (qtext.empty() ? "?" : qtext) << std::endl;
            std::map<std::string, std::string> answerTexts;
            std::map<std::string, std::vector<std::string>> voters;
            auto ans = pev.content.find("answers");
            if (ans != pev.content.end() && ans->is_array()) {
                for (const auto& a : *ans) {
                    if (!a.is_object()) continue;
                    std::string id = a.value("id", "");
                    std::string text = a.value("text", "");
                    if (!id.empty()) answerTexts[id] = text;
                }
            }
            for (const auto& ev2 : st.messages) {
                if (ev2.content.value("msgtype", "") != "m.poll.response") continue;
                auto rel2 = ev2.content.find("m.relates_to");
                if (rel2 == ev2.content.end() || !rel2->is_object()) continue;
                if (rel2->value("event_id", "") != pev.event_id) continue;
                auto sel = ev2.content.find("selections");
                if (sel == ev2.content.end() || !sel->is_array() ||
                    sel->empty() || !(*sel)[0].is_string()) continue;
                voters[(*sel)[0].get<std::string>()].push_back(
                    senderShort(ev2.sender));
            }
            int shown = 0;
            for (const auto& [id, votersList] : voters) {
                auto it = answerTexts.find(id);
                std::string label = (it != answerTexts.end() && !it->second.empty())
                                        ? it->second : id;
                std::string who;
                for (const auto& v : votersList) {
                    if (!who.empty()) who += ", ";
                    who += v;
                }
                std::cout << "  (" << id << ") " << label << " — "
                          << votersList.size() << " vote"
                          << (votersList.size() == 1 ? "" : "s")
                          << (who.empty() ? "" : " (" + who + ")") << std::endl;
                shown++;
            }
            if (shown == 0) {
                std::cout << "  No votes yet." << std::endl;
            }
            return true;
        }
        // ---- settings: the current client settings ----
        if (a.command == "settings") {
            // The optional keyword filters the list (settings <keyword>).
            std::string kw;
            for (const auto& w : a.positional) kw += (kw.empty() ? "" : " ") + w;
            auto kwMatch = [&](const std::string& line) {
                if (kw.empty()) return true;
                std::string l = line, k = kw;
                std::transform(l.begin(), l.end(), l.begin(), ::tolower);
                std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                return l.find(k) != std::string::npos;
            };
            std::vector<std::string> lines;
            auto add = [&](const std::string& line) {
                if (kwMatch(line)) lines.push_back(line);
            };
            add("Settings — Element equivalents in parentheses:");
            add("  time      " + std::string(st.showSeconds ? "HH:MM:SS" : "HH:MM") + "       (time full / time off)");
            add("  clock     " + std::string(st.clock12h ? "12h (AM/PM)" : "24h") + "       (clock 12h / clock 24h)  [Element: 24-hour clock]");
            add("  timeside  " + std::string(st.timeRight ? "right" : "left") + "      (timeside left / timeside right)");
            add("  msgline   " + std::string(st.msgNewline ? "newline (message below)" : "inline (same line)") + "  (msgline inline / msgline newline)");
            add("  agent     " + dbi.getSetting("agent_provider", "openai") + " / " + dbi.getSetting("agent_model", "(default model)") + " / trust: " + dbi.getSetting("agent_trust", "ask") + "  (agent config <k> <v> / agent trust <allow|ask|deny>)");
            add("  nickname  " + dbi.getSetting("displayname", "(unset)") + "  (nickname <name>)");
            add("  avatar    " + dbi.getSetting("avatar_url", "(unset)") + "  (avatar <url>)");
            add("  presence  " + dbi.getSetting("presence", "online") + "  (presence online|away|offline)");
            add("  names     " + std::string(st.showNames ? "shown" : "hidden") + "       (names on / names off)  [Element: show sender display names]");
            add("  receipts  " + std::string(st.showReceipts ? "shown" : "hidden") + "  (receipts on / receipts off)  [Element: show read receipts]");
            add("  sendrcpts " + std::string(dbi.receiptsEnabled(st.currentRoomId) ? "on" : "off") + " (this room)  (receipts send on / receipts send off)  [Element: send read receipts]");
            add("  modredact " + std::string(dbi.getSetting("mod_redact", "0") == "1" ? "allowed" : "blocked") + "  (modredact on / modredact off)  [redact others' messages]");
            add("  sendtyping " + std::string(dbi.getSetting("send_typing", "1") != "0" ? "on" : "off") + "       (sendtyping on / sendtyping off)  [Element: send typing notifications]");
            add("  invreason chat " + std::string(dbi.getSetting("invreason_chat", "1") != "0" ? "shown" : "hidden") + "  (invreason chat on|off)");
            add("  invreason menu " + std::string(dbi.getSetting("invreason_menu", "0") != "0" ? "shown" : "hidden") + "  (invreason menu on|off)");
            add("  joins     " + std::string(st.showJoins ? "shown" : "hidden") + "       (joins on / joins off)  [Element: show join/leave messages]");
            add("  links     " + std::string(st.showLinks ? "pills" : "raw URLs") + "      (links on / links off)  [Element: URL previews]");
            add("  ids       " + std::string(st.showIds ? "shown" : "hidden") + "       (ids on / ids off)  [Element: developer mode]");
            add("  images    " + std::string(st.showImages ? "full cards" : "compact") + "  (images on / images off)  [Element: show images & videos]");
            add("  sendpreset " + dbi.getSetting("send_preset", "original") + "  (sendpreset original|compact|full)");
            add("  emoji     " + std::string(st.showEmoji ? "on" : "off (ASCII)") + "       (emoji on / emoji off)");
            add("  threads   " + std::string(dbi.getSetting("threads_off", "0") == "0" ? "enabled" : "disabled") + "  (threads on / threads off)");
            add("  rows      " + (st.limitRows > 0 ? std::to_string(st.limitRows) : "auto (terminal)") + "  (rows <n> / rows 0)");
            add("  panel L   " + (st.leftPanelW == 0 ? "off" : st.leftPanelW > 0 ? std::to_string(st.leftPanelW) : "default") + "  (panel left <off|on|width>)");
            add("  panel R   " + (st.rightPanelW == 0 ? "off" : st.rightPanelW > 0 ? std::to_string(st.rightPanelW) : "default") + "  (panel right <off|on|width>)");
            add("  panels    " + std::string(st.autoPanels ? "auto (sized to content)" : "fixed") + "  (panel auto on / panel auto off)");
            add("  members   " + std::string(st.membersMode == 1 ? "horizontal" : st.membersMode == 2 ? "vertical list" : "auto") + "  (members <horizontal|list|auto>)");
            add("  via       " + (st.viaLimit == 0 ? "unlimited (all servers)" : std::to_string(st.viaLimit)) + "  (via <n> / via 0) [Element: 3]");
            add("  timezone  " + std::string(st.tzOffset >= 0 ? "+" : "") + std::to_string(st.tzOffset) + "h  (timezone <N>)");
            add("  hide      " + std::to_string(st.hiddenSeconds) + "s  (hide <room> [seconds])");
            add("  from      " + (st.senderFilter.empty() ? "all" : st.senderFilter) + "  (from <@user> / from off)");
            add("  muted     " + std::to_string(st.mutedRooms.size()) + " rooms  (mute <room> on|off)");
            add("  layout    " + std::string(st.mobile ? "smartphone (stacked)" : "desktop (three columns)") + "  (mobile on / mobile off)");
            add("  account   " + st.accountLabel);
            add("  proxy     " + st.proxyLabel);
            add("  invites   " + std::string(st.showInvites ? "on" : "off") + "  (invites on / invites off)  [" + std::to_string(st.invites) + " open]");
            add("  notif     " + std::string(st.showNotifications ? "on" : "off") + "  (notifications on / notifications off)  [bottom-right corner]");
            {
                std::string mon;
                for (const auto& r : st.rooms) {
                    std::string m = dbi.getSetting(
                        "monitor:" + r.value("room_id", ""), "0");
                    if (m != "0" && m != "off")
                        mon += (mon.empty() ? "" : ", ") + r.value("name", "?")
                             + " " + m + "%";
                }
                add("  monitor   " + (mon.empty() ? "none" : mon)
                    + "  (monitor <room> <0-100|off>  [receipts notify at 100%])");
            }
            add("  space     " + (st.activeSpace.empty() ? "all rooms" : st.activeSpace) + "  (space <name> / space all)");
            for (const auto& l : lines) std::cout << l << std::endl;
            return true;
        }
        // ---- threads on|off: disable the thread UI entirely ----
        if (a.command == "threads") {
            if (!a.positional.empty() &&
                (a.positional[0] == "bottom" || a.positional[0] == "panel")) {
                st.showThreadsBottom = a.positional.size() < 2 || a.positional[1] != "off";
                dbi.setSetting("threads_bottom", st.showThreadsBottom ? "1" : "0");
                st.statusNote = std::string("the threads bottom list: ") + (st.showThreadsBottom ? "on" : "off");
            } else {
                const bool on = !a.positional.empty() && a.positional[0] == "on";
                dbi.setSetting("threads_off", on ? "0" : "1");
                st.statusNote = std::string("threads: ") + (on ? "enabled" : "disabled (the markers hidden)");
            }
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- invreason chat|menu on|off: where the invitation reasons show ----
        if (a.command == "invreason") {
            if (a.positional.size() >= 2 &&
                (a.positional[0] == "chat" || a.positional[0] == "menu")) {
                const std::string key = std::string("invreason_") + a.positional[0];
                const bool on = a.positional[1] != "off";
                dbi.setSetting(key, on ? "1" : "0");
                st.statusNote = std::string("the invite reason (") + a.positional[0]
                              + "): " + (on ? "shown" : "hidden");
            } else {
                std::cout << "Usage: invreason <chat|menu> <on|off>" << std::endl;
            }
            return true;
        }
        // ---- sendpreset original|compact|full: the media sending preset ----
        if (a.command == "sendpreset") {
            std::string v = a.positional.empty() ? "original" : a.positional[0];
            if (v != "original" && v != "compact" && v != "full") v = "original";
            dbi.setSetting("send_preset", v);
            st.statusNote = "the send preset: " + v;
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        // ---- nickname / avatar / presence: the account options ----
        if (a.command == "presence" &&
            !a.positional.empty() &&
            (a.positional[0] == "online" || a.positional[0] == "away" ||
             a.positional[0] == "offline")) {
            // The set-form only; the bare 'presence' fetches the members'
            // presence (the other handler).
            st.statusNote = "presence set: " + a.positional[0];
            dbi.setSetting("presence", a.positional[0]);
            if (pcore::init() && pcore::loadSavedSession()) {
                matrix::Client cl;
                db::StoredAccount sacc = dbi.loadAccount();
                if (sacc.is_logged_in()) {
                    cl.setHomeserverURL(sacc.homeserver_url);
                    cl.setAccessToken(sacc.access_token);
                    try { cl.setPresence(a.positional[0]); } catch (...) {}
                }
            }
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
        if (a.command == "nickname" || a.command == "avatar") {
            if (a.positional.empty()) {
                std::cout << "Usage: " << a.command
                          << (a.command == "nickname" ? " <name>" : a.command == "avatar" ? " <url>" : " <online|away|offline>")
                          << std::endl;
                return true;
            }
            const std::string v = a.positional[0];
            dbi.setSetting(a.command == "nickname" ? "displayname"
                                                        : "avatar_url", v);
            if (pcore::init() && pcore::loadSavedSession()) {
                auto& client = pcore::core().client;
                if (a.command == "nickname") client->setDisplayName(v);
                else client->setAvatarUrl(v);
            }
            st.statusNote = a.command + " set: " + v;
            std::cout << drawFrame(st) << std::flush;
            return true;
        }
    return false;
}} // namespace matrixcli

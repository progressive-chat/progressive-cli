// src/ascii_ui.cpp — ASCII-drawn client interface for the CLI (not the TUI).
//
// `progressive-cli ui` draws a chat-client-like layout with plain characters:
// a header, a left panel with the room list, the open room's messages in
// the center and the member list on the right, panels separated by pipes.
// It is a REPL: every command executes and the whole frame is redrawn —
// no auto-updates, no raw terminal mode (works in any terminal, scrolls
// like a normal CLI program).
#include "ascii_ui.hpp"
#include "ascii_state.hpp"
#include "commands.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/string_utils.hpp"
#include "agent_tools.hpp"
#include <cstdlib>
#include <glob.h>
#include <poll.h>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "cli/args.hpp"
#include "pcore.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>
#include <unordered_map>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

#include "ascii_ui_impl.hpp"

namespace matrixcli {

std::string drawFrameChatImpl(const UiState& st, int centerW, bool horizMembers,
                              int W, int leftW, int rightW, int scroll,
                              int rows, const char* PIPE, const char* X,
                              std::string out,
                              const std::vector<const nlohmann::json*>& visible,
                              const std::string& roomName) {
    // them like a real timeline.
    std::vector<std::string> centerRows;
   
        // Horizontal members: one dim row across the top of the chat.
        if (horizMembers && !st.members.empty()) {
            std::string mrow;
            for (const auto& mem : st.members) {
                if (!mrow.empty()) mrow += " · ";
                mrow += memberRowStr(st, mem);
            }
            centerRows.push_back("[90m" + clip(mrow, centerW - 2) + "[0m");
            // The thread list follows as a second horizontal row.
            if (st.showThreadsBottom && !st.currentRoomId.empty()) {
                auto evs = st.db->getEvents(st.currentRoomId, 300);
                std::string trow;
                for (const auto& ev : evs) {
                    int rc = 0;
                    for (const auto& ev2 : evs) {
                        if (eventThreadRoot(ev2) == ev.event_id) rc++;
                    }
                    if (rc > 0) {
                        if (!trow.empty()) trow += "  ";
                        trow += "⤷ " + clip(eventBodyImpl(ev), 20) + " ("
                              + std::to_string(rc) + ")";
                    }
                }
                if (!trow.empty()) {
                    centerRows.push_back("[90m" + clip(trow, centerW - 2)
                                         + "[0m");
                }
            }
        }
 {
        auto renderRow = [&](const matrix::Event& ev) -> std::string {
            std::string center;
            std::string body = eventBodyRaw(ev);
            // Membership events render as "joined/left the room" rows.
            if (ev.type == "m.room.member" && ev.content.is_object()) {
                auto m = ev.content.find("membership");
                if (m != ev.content.end() && m->is_string()) {
                    std::string ms = m->get<std::string>();
                    std::string who = chatName(st, st.currentRoomId, ev.sender);
                    if (ms == "join") center = "[" + who + "] joined the room";
                    else if (ms == "leave") center = "[" + who + "] left the room";
                    else if (ms == "invite") {
                        std::string target = ev.state_key.empty()
                                                 ? "" : senderShortImpl(ev.state_key);
                        std::string reason = ev.content.value("reason", "");
                        center = "[" + who + "] invited "
                               + (target.empty() ? "someone" : target);
                        if (!reason.empty() &&
                            st.db->getSetting("invreason_chat", "1") != "0") {
                            center += " — " + reason;
                        }
                    }
                    else if (ms == "ban") center = "[" + who + "] was banned";
                    else center = "[" + who + "] membership: " + ms;
                }
                if (center.empty()) center = "[" + senderShortImpl(ev.sender) + "] (member event)";
                return center;
            }
            // Extra content types: polls, stickers, audio, video, images.
            if (ev.type == "m.room.message" && ev.content.is_object()) {
                auto mtIt = ev.content.find("msgtype");
                std::string mt = mtIt != ev.content.end() && mtIt->is_string()
                                     ? mtIt->get<std::string>() : "";
                if (mt == "m.poll.start") {
                    auto q = ev.content.find("question");
                    std::string qtext;
                    if (q != ev.content.end() && q->is_object()) {
                        auto t = q->find("text");
                        if (t != q->end() && t->is_string()) qtext = t->get<std::string>();
                    }
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u2b55 poll: " : "[poll] ")
                           + (qtext.empty() ? "?" : qtext);
                } else if (mt == "m.poll.response") {
                    // A poll vote — the event has no body, render it as a
                    // vote row instead of an empty "[alice]".
                    auto rel = ev.content.find("m.relates_to");
                    std::string target;
                    if (rel != ev.content.end() && rel->is_object()) {
                        target = rel->value("event_id", "");
                    }
                    // The vote row uses plain text — the 🗳 glyph renders
                    // one cell in the phone terminal and shifted the pipes.
                    center = "[" + senderShortImpl(ev.sender) + "] voted";
                    // The picked options: the selections carry the answer
                    // ids; map them to the option texts of the poll start
                    // event so the row shows the choice directly.
                    matrix::Event pev;
                    if (st.db && st.db->getEventById(target, pev)
                        && pev.content.is_object()) {
                        std::string chosen;
                        auto sel = ev.content.find("selections");
                        auto ans = pev.content.find("answers");
                        if (sel != ev.content.end() && sel->is_array()
                            && ans != pev.content.end() && ans->is_array()) {
                            for (const auto& sid : *sel) {
                                if (!sid.is_string()) continue;
                                for (const auto& av : *ans) {
                                    if (!av.is_object()) continue;
                                    if (av.value("id", "")
                                        == sid.get<std::string>()) {
                                        std::string txt = av.value("text", "");
                                        if (!txt.empty()) {
                                            if (!chosen.empty())
                                                chosen += ", ";
                                            chosen += txt;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        if (!chosen.empty())
                            center += " \x1b[36m" + chosen + "\x1b[0m";
                        // A dim hint under the row: which voting this vote
                        // belongs to, so the chat shows what is being voted
                        // about at a glance.
                        auto q = pev.content.find("question");
                        if (q != pev.content.end() && q->is_object()) {
                            std::string qtext = q->value("text", "");
                            if (!qtext.empty()) {
                                center += "\n  \x1b[90m"
                                        + clip(qtext, std::max(20, centerW - 4))
                                        + "\x1b[0m";
                            }
                        }
                    }
                } else if (mt == "m.sticker") {
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u2b1c sticker: " : "[sticker] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.audio") {
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u266a audio: " : "[audio] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.file") {
                    center = senderTag(st, st.currentRoomId, ev.sender)
                           + (st.showEmoji ? "\xf0\x9f\x93\x84 " : "[file] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.video") {
                    center = "[" + senderShortImpl(ev.sender) + "] "
                           + (st.showEmoji ? "\u25b6 video: " : "[video] ")
                           + (body.empty() ? "?" : "\x1b[36m" + body + "\x1b[0m");
                } else if (mt == "m.image") {
                    std::string dims;
                    auto info = ev.content.find("info");
                    if (info != ev.content.end() && info->is_object()) {
                        auto w = info->find("w");
                        auto h = info->find("h");
                        if (w != info->end() && h != info->end() &&
                            w->is_number() && h->is_number()) {
                            dims = " (" + std::to_string(w->get<int>()) + "x"
                                 + std::to_string(h->get<int>()) + ")";
                        }
                    }
                    // Images are compact by default; 'images on' shows the
                    // full card (dims).
                    std::string imgPrefix = st.showImages
                        ? (st.showEmoji ? "\u2b1c image: " : "[img] ")
                        : (st.showEmoji ? "\xf0\x9f\x96\xbc " : "[img] ");
                    center = "[" + senderShortImpl(ev.sender) + "] " + imgPrefix
                           + "\x1b[36m" + body + "\x1b[0m" + dims;
                }
            }
            std::string thr = eventThreadRoot(ev);
            std::string rep = eventReplyTo(ev);
            if (center.empty() && !thr.empty()) {
                std::string preview = eventPreview(st.db, st.currentRoomId, thr);
                center = "[" + chatName(st, st.currentRoomId, ev.sender) + "] \u2937 " + body
                       + "  (thread: " + (preview.empty() ? thr : preview) + ")";
            } else if (center.empty() && !rep.empty()) {
                // Element-style ReplyChain: walk the m.in_reply_to chain
                // (up to 3 levels) and stack the quoted previews, each
                // indented — the reply-of-reply shows both ancestors.
                std::string chain;
                std::string cur = rep;
                for (int lvl = 0; lvl < 3; ++lvl) {
                    matrix::Event prev;
                    if (!st.db->getEventById(cur, prev)) break;
                    std::string preview = eventBodyImpl(prev);
                    if (preview.empty()) break;
                    // The chain line uses the full effective panel width
                    // (the whole screen in mobile) — the wrap takes care of
                    // any overflow.
                    int chainW = st.mobile ? W : centerW;
                    chain += std::string(lvl, ' ') + "> ["
                           + chatName(st, st.currentRoomId, prev.sender)
                           + "] " + clip(preview, std::max(20, chainW)) + "\n";
                    auto rel = prev.content.find("m.relates_to");
                    if (rel == prev.content.end() || !rel->is_object()) break;
                    auto ir = rel->find("m.in_reply_to");
                    if (ir == rel->end() || !ir->is_object()) break;
                    auto eid = ir->find("event_id");
                    if (eid == ir->end() || !eid->is_string()) break;
                    cur = eid->get<std::string>();
                }
                center = "[" + chatName(st, st.currentRoomId, ev.sender) + "] " + body
                       + (chain.empty() ? "" : "\n" + chain);
            } else if (center.empty()) {
                center = senderTag(st, st.currentRoomId, ev.sender)
                       + (st.showLinks
                              ? highlightUrls(renderPermalinks(
                                    highlightMentions(
                                        renderMarkdownBody(body)),
                                    st.rooms, st.db))
                              : highlightMentions(renderMarkdownBody(body)));
                int rc = 0;
                for (const auto& ev2 : st.messages) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0) center += "  \u2937(" + std::to_string(rc) + ")";
            }
            // Redaction events render as system lines.
            if (ev.type == "m.room.redaction") {
                return "\xf0\x9f\x97\x91 " + senderShortImpl(ev.sender)
                     + " deleted a message";  // 🗑
            }
            // Redacted targets get the (deleted) marker.
            if (st.redactedIds.count(ev.event_id)) {
                center += "  (\xf0\x9f\x97\x91 deleted)";
            }
            // Poll response counts under the poll — with the option texts.
            if (!center.empty() &&
                center.find("poll:") != std::string::npos && ev.content.is_object()) {
                std::map<std::string, std::string> answerTexts;
                auto ans = ev.content.find("answers");
                if (ans != ev.content.end() && ans->is_array()) {
                    for (const auto& a : *ans) {
                        if (!a.is_object()) continue;
                        std::string id = a.value("id", "");
                        std::string text = a.value("text", "");
                        if (!id.empty()) answerTexts[id] = text;
                    }
                }
                std::map<std::string, int> votes;
                for (const auto& ev2 : st.messages) {
                    if (!ev2.content.is_object()) continue;
                    auto mt2 = ev2.content.find("msgtype");
                    if (mt2 == ev2.content.end() || !mt2->is_string() ||
                        mt2->get<std::string>() != "m.poll.response") continue;
                    auto rel2 = ev2.content.find("m.relates_to");
                    if (rel2 == ev2.content.end() || !rel2->is_object()) continue;
                    auto eid2 = rel2->find("event_id");
                    if (eid2 == rel2->end() || !eid2->is_string() ||
                        eid2->get<std::string>() != ev.event_id) continue;
                    auto sel = ev2.content.find("selections");
                    if (sel != ev2.content.end() && sel->is_array() &&
                        !sel->empty() && (*sel)[0].is_string()) {
                        votes[(*sel)[0].get<std::string>()]++;
                    }
                }
                if (!votes.empty()) {
                    std::string vstr;
                    for (const auto& [k, n] : votes) {
                        if (!vstr.empty()) vstr += ", ";
                        auto it = answerTexts.find(k);
                        std::string label = (it != answerTexts.end() && !it->second.empty())
                                                ? it->second : k;
                        vstr += label + ": " + std::to_string(n);
                    }
                    center += "  \xf0\x9f\x97\xb3 " + vstr;  // 🗳
                }
            }
            // Reactions and edits aggregated for EVERY message row.
            std::map<std::string, int> reacts;
            bool edited = false;
            for (const auto& ev2 : st.messages) {
                if (!ev2.content.is_object()) continue;
                auto rel = ev2.content.find("m.relates_to");
                if (rel == ev2.content.end() || !rel->is_object()) continue;
                auto rtype = rel->find("rel_type");
                auto teid = rel->find("event_id");
                if (rtype == rel->end() || teid == rel->end() ||
                    !rtype->is_string() || !teid->is_string()) continue;
                if (teid->get<std::string>() != ev.event_id) continue;
                std::string rt = rtype->get<std::string>();
                if (rt == "m.annotation") {
                    auto key = rel->find("key");
                    if (key != rel->end() && key->is_string()) {
                        reacts[key->get<std::string>()]++;
                    }
                } else if (rt == "m.replace") {
                    edited = true;
                }
            }
            for (const auto& [k, n] : reacts) {
                center += "  \x1b[32m" + k + " " + std::to_string(n) + "\x1b[0m";
            }
            if (edited) center += "  (edited)";
            return center;
        };
        int64_t prevDay = -1;
        // The last-read divider: the messages AFTER the marker count as
        // "new" (Element-style).
        int newAfterMarker = 0;
        if (!st.readMarker.empty()) {
            bool past = false;
            for (const auto& ev2 : st.messages) {
                if (!past) {
                    if (ev2.event_id == st.readMarker) past = true;
                    continue;
                }
                if (ev2.type == "m.room.message" || ev2.type == "m.sticker")
                    newAfterMarker++;
            }
        }
        // The pinned banner and the upgrade banner — one slim row at the
        // top of the chat, before the day separators.
        if (!st.mobile) {
            if (!st.pinned.empty()) {
                std::string b = st.showEmoji
                    ? "\xf0\x9f\x93\x8c " : "[pins] ";
                b += std::to_string(st.pinned.size())
                   + (st.pinned.size() == 1 ? " pinned message" : " pinned messages")
                   + " \x1b[90m\u2014 'pins' to list, 'goto <short-id>' to jump\x1b[0m";
                centerRows.push_back(clip(b, centerW - 1));
            }
            std::string succ = tombstoneSuccessor(st.messages);
            if (!succ.empty()) {
                std::string b = st.showEmoji
                    ? "\xe2\x99\xbb " : "[upgraded] ";
                b += "this room was upgraded \xe2\x86\x92 moved to " + succ
                   + "  \x1b[90m(goto successor)\x1b[0m";
                centerRows.push_back(clip(b, centerW - 1));
            }
        }
        for (const auto& ev : st.messages) {
            int64_t day = ev.origin_server_ts / 86400000;
            if (day != prevDay) {
                std::string label;
                std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000);
                std::tm tm{};
                localtime_r(&t, &tm);
                std::time_t nowT = std::time(nullptr);
                std::tm nowTm{};
                localtime_r(&nowT, &nowTm);
                char dateBuf[16];
                std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
                std::string dateStr(dateBuf);
                if (tm.tm_year == nowTm.tm_year && tm.tm_yday == nowTm.tm_yday) {
                    label = "Today (" + dateStr + ")";
                } else if (tm.tm_year == nowTm.tm_year && tm.tm_yday == nowTm.tm_yday - 1) {
                    label = "Yesterday (" + dateStr + ")";
                } else {
                    label = dateStr;
                }
                // The day's message count next to the date, like Element.
                int dayCount = 0;
                for (const auto& ev2 : st.messages) {
                    if (ev2.origin_server_ts / 86400000 != day) continue;
                    if (ev2.type != "m.room.message" && ev2.type != "m.sticker") continue;
                    dayCount++;
                }
                std::string sep = "── " + label + " ──  " + std::to_string(dayCount)
                                + " msgs";
                if (static_cast<int>(sep.size()) < centerW) {
                    sep = std::string((centerW - static_cast<int>(sep.size())) / 2, ' ') + sep;
                }
                centerRows.push_back(sep);
                prevDay = day;
            }
            // Element-style "viewing an earlier message" banner, right
            // above the jumped-to event.
            if (!st.focusEvent.empty() && ev.event_id == st.focusEvent) {
                std::string bar = "[44m â viewing event \xe2\x80\xb9"
                                + clip(st.focusEvent, 24) + "\xe2\x80\xba"
                                + " · 'newest' to return [0m";
                if (static_cast<int>(bar.size()) < centerW) {
                    bar = std::string(std::max(0, (centerW - static_cast<int>(bar.size())) / 2), ' ')
                        + bar;
                }
                centerRows.push_back(bar);
            }
            // The last-read divider: where the user last read up to, with
            // the count of the newer messages (the local m.fully_read copy).
            if (!st.readMarker.empty() && ev.event_id == st.readMarker) {
                std::string mark = "\x1b[33m── last read ──\x1b[0m";
                if (newAfterMarker > 0) {
                    mark += "  \x1b[90m" + std::to_string(newAfterMarker)
                          + " new\x1b[0m";
                }
                centerRows.push_back(mark);
            }
            // Element "show join/leave messages": member events are system
            // rows — hidden when the setting is off.
            if (!st.senderFilter.empty() && ev.type == "m.room.message" &&
                ev.sender != st.senderFilter &&
                ev.sender != "@" + st.senderFilter.substr(1)) continue;
            if (!st.showJoins && ev.type == "m.room.member") continue;
            // Reactions are aggregated into the message rows; the state
            // events (power levels, encryption, room meta) are system
            // data — neither should become standalone (empty) lines.
            // Tombstones (room upgraded): a dim system row with the
            // successor — the banner at the top has the full line.
            if (ev.type == "m.room.tombstone") {
                std::string succ = tombstoneSuccessor(st.messages);
                centerRows.push_back("\x1b[90m\u266b room upgraded \xe2\x86\x92 successor "
                    + (succ.empty() ? "\xfffd" : succ) + " \x1b[0m");
                continue;
            }
            if (ev.type == "m.reaction" || ev.type == "m.room.power_levels" ||
                ev.type == "m.room.encryption" || ev.type == "m.room.create" ||
                ev.type == "m.room.topic" || ev.type == "m.room.name" ||
                ev.type == "m.room.avatar" || ev.type == "m.room.canonical_alias" ||
                ev.type == "m.room.join_rules" ||
                ev.type == "m.room.history_visibility" ||
                ev.type == "m.room.encrypted" || ev.type == "m.room.redaction" ||
                ev.type == "m.receipt") {  // ephemeral — the corner shows them
                continue;
            }
            std::string row = renderRow(ev);
            if (!st.showNames) {
                // Element compact mode: hide the sender nicknames.
                if (row.size() >= 2 && row[0] == '[') {
                    auto close = row.find(']');
                    if (close != std::string::npos && close + 2 <= row.size() &&
                        row[close + 1] == ' ') {
                        row = row.substr(close + 2);
                    }
                }
            }
            if (!row.empty()) {
                auto rIt = st.receipts.find(ev.event_id);
                if (st.showReceipts && rIt != st.receipts.end() && !rIt->second.empty()) {
                    std::string rd = rIt->second;
                    // Cap the reader list: 'a b c +5' instead of the full set.
                    std::string shown;
                    int count = 0;
                    std::istringstream rss(rd);
                    std::string tok;
                    while (rss >> tok && count < 3) {
                        if (!shown.empty()) shown += " ";
                        shown += tok;
                        count++;
                    }
                    int total = 1;
                    for (char ch : rd) if (ch == ' ') total++;
                    if (count < total) shown += " +" + std::to_string(total - count);
                    row += "  \x1b[90m\u2713 " + shown + "\x1b[0m";  // ✓ readers
                }
                std::time_t t = static_cast<std::time_t>(ev.origin_server_ts / 1000)
                               + static_cast<std::time_t>(st.tzOffset) * 3600;
                std::tm tm{};
                localtime_r(&t, &tm);
                char buf[16];
                int h12 = tm.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                const char* ap = tm.tm_hour < 12 ? "AM" : "PM";
                if (st.clock12h) {
                    if (st.showSeconds) {
                        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d %s",
                                      h12, tm.tm_min, tm.tm_sec, ap);
                    } else {
                        std::snprintf(buf, sizeof(buf), "%d:%02d %s",
                                      h12, tm.tm_min, ap);
                    }
                } else if (st.showSeconds) {
                    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                                  tm.tm_hour, tm.tm_min, tm.tm_sec);
                } else {
                    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                                  tm.tm_hour, tm.tm_min);
                }
                std::string timeStr = buf;
                std::string head, body;
                if (st.msgNewline) {
                    // "HH:MM [nick] >" on its own line, the message below
                    // (WhatsApp-like) — only text rows have the "> " split.
                    auto gt = row.find("> ");
                    if (gt != std::string::npos) {
                        head = row.substr(0, gt + 1);
                        body = row.substr(gt + 2);
                    } else {
                        head = row;
                    }
                }
                std::string prefix;
                if (st.pinned.count(ev.event_id)) {
                    prefix += (st.showEmoji ? "\xf0\x9f\x93\x8c " : "[pin] ");
                }
                if (ev.event_id == st.focusEvent) {
                    prefix += "\x1b[7m \xe2\x97\x80 \x1b[0m";
                }
                std::string idSuffix;
                if (st.showIds) {
                    std::string shortId = ev.event_id.substr(0, 10);
                    if (!shortId.empty()) idSuffix = "  \u2039" + shortId + "\u203a";
                }
                // Long messages wrap to the panel width (word-aware, ANSI
                // safe); the continuation lines are indented under the
                // first one.
                // Wrap to the panel width minus the 8-cell continuation
                // indent, so indented lines never overflow the panel and
                // words are never cut at the edge.
                int wrapW = (st.mobile ? W : centerW) - 8;
                if (st.msgNewline && !body.empty()) {
                    std::string line0;
                    if (st.timeRight) {
                        std::string leftPart = prefix + head + idSuffix;
                        int padN = wrapW + 8 - displayWidth(leftPart)
                                 - displayWidth(timeStr);
                        line0 = leftPart + std::string(std::max(0, padN), ' ')
                              + timeStr;
                    } else {
                        line0 = prefix + timeStr + " " + head + idSuffix;
                    }
                    centerRows.push_back(line0);
                    auto bLines = wrapTextImpl(body, wrapW);
                    for (const auto& bl : bLines) {
                        centerRows.push_back(std::string(8, ' ') + bl);
                    }
                } else if (st.timeRight) {
                    // Reserve the time at the first line's right edge.
                    std::string inner = prefix + row + idSuffix;
                    int reserve = displayWidth(timeStr) + 1;
                    auto tLines = wrapTextImpl(inner, std::max(8, wrapW - reserve));
                    for (size_t li = 0; li < tLines.size(); ++li) {
                        if (li == 0) {
                            int padN = wrapW + 8 - displayWidth(tLines[0])
                                     - displayWidth(timeStr);
                            centerRows.push_back(tLines[0]
                                + std::string(std::max(0, padN), ' ') + timeStr);
                        } else {
                            centerRows.push_back(std::string(8, ' ') + tLines[li]);
                        }
                    }
                } else {
                    std::string first = prefix + timeStr + " " + row + idSuffix;
                    std::vector<std::string> lines = wrapTextImpl(first, wrapW);
                    for (size_t li = 0; li < lines.size(); ++li) {
                        if (li == 0) {
                            centerRows.push_back(lines[0]);
                        } else {
                            centerRows.push_back(std::string(8, ' ') + lines[li]);
                        }
                    }
                }
                // Thread master messages: their full id on its own line
                // (with --ids), so the roots are easy to reference.
                if (st.showIds && !ev.event_id.empty() &&
                    eventThreadRoot(ev) == ev.event_id) {
                    centerRows.push_back("      \xe2\xa4\xb7 thread root: "
                                         + clip(ev.event_id, centerW - 16));
                }
            }
        }
    }
    // Right panel content per mode: members | room thread list | one thread
    // | threads across all rooms (Element-style).
    std::vector<std::string> rightRows;
    if (st.rightPanel == 0) {
        // Full @user:server ids when the panel has the room for them.
        bool fullIds = false;
        if (!st.members.empty() && rightW >= 15) {
            int fullW = 0;
            for (const auto& mem : st.members) {
                int w = displayWidth(memberRowStr(st, mem, true));
                if (w > fullW) fullW = w;
            }
            fullIds = (rightW - 2 >= fullW);
        }
        for (const auto& mem : st.members) {
            rightRows.push_back(memberRowStr(st, mem, fullIds));
        }
        // The thread list sits at the BOTTOM of the right panel, under a
        // separator (Element-style): ⤷ <preview> (<reply count>).
        // The members get the top rows, the threads a window at the bottom
        // with its own scroll (--scroll-threads).
        if (st.showThreadsBottom && !st.currentRoomId.empty()) {
            std::vector<std::string> thr =
                roomThreadList(st.db, st.currentRoomId, rightW - 7, st.showIds);
            if (!thr.empty()) {
                // Reserve room for the threads: the member list gets capped
                // so the thread window (at least 1 row) always fits.
                int membersShown = static_cast<int>(rightRows.size());
                int wantThr = std::min(static_cast<int>(thr.size()),
                                       std::max(1, rows / 5));
                int membersCap = std::max(0, rows - wantThr - 1);
                if (membersShown > membersCap) rightRows.resize(membersCap);
                int thrWindow = std::max(1, rows - membersShown - 1);
                int ts = std::min(std::max(0, st.threadsScroll),
                                  std::max(0, static_cast<int>(thr.size()) - thrWindow));
                rightRows.push_back("----------");
                for (int k = 0; k < thrWindow && ts + k < static_cast<int>(thr.size());
                     ++k) {
                    rightRows.push_back(thr[static_cast<size_t>(ts + k)]);
                }
            }
        }
    } else if (st.rightPanel == 1) {
        // The room's threads: the roots with their reply counts (the
        // numbers select them: thread <room> <N>).
        auto evs = st.db->getEvents(st.threadRoomId, 300);
        int idx = 0;
        for (const auto& ev : evs) {
            int rc = 0;
            for (const auto& ev2 : evs) {
                if (eventThreadRoot(ev2) == ev.event_id) rc++;
            }
            if (rc > 0) {
                rightRows.push_back(std::to_string(idx + 1) + " \u2937 "
                                    + clip(eventBodyImpl(ev), 20) + " (" +
                                    std::to_string(rc) + ")");
                idx++;
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no threads in this room)");
    } else if (st.rightPanel == 2) {
        // One thread: the root + its replies (Element's thread panel),
        // wrapped to the panel width so long lines continue below.
        auto pushWrapped = [&](const std::string& line) {
            auto lines = wrapTextImpl(line, std::max(8, rightW - 1));
            for (size_t li = 0; li < lines.size(); ++li) {
                rightRows.push_back((li == 0 ? std::string()
                                             : std::string(4, ' ')) + lines[li]);
            }
        };
        if (!st.threadRootId.empty()) {
            for (const auto& ev : st.messages) {
                if (ev.event_id == st.threadRootId) {
                    pushWrapped("[" + senderShortImpl(ev.sender) + "] "
                                + eventBodyImpl(ev));
                }
            }
            for (const auto& ev : st.threadReplies) {
                pushWrapped("  \u2937 [" + senderShortImpl(ev.sender) + "] "
                            + eventBodyImpl(ev));
            }
        }
        if (rightRows.empty()) rightRows.push_back("(thread is empty)");
        // A small thread leaves free rows: fill them with the OTHER
        // threads and the members, so everything sits in one panel.
        if (static_cast<int>(rightRows.size()) < rows) {
            auto evs = st.db->getEvents(st.threadRoomId, 300);
            std::vector<std::string> thr;
            int idx = 0;
            for (const auto& ev : evs) {
                int rc = 0;
                for (const auto& ev2 : evs) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0) {
                    idx++;
                    if (ev.event_id == st.threadRootId) continue;
                    thr.push_back(std::to_string(idx) + " \u2937 "
                                  + clip(eventBodyImpl(ev), 20) + " (" +
                                  std::to_string(rc) + ")");
                }
            }
            int free = rows - static_cast<int>(rightRows.size());
            if (!thr.empty() && free > 3) {
                int thrN = std::min(static_cast<int>(thr.size()),
                                    std::max(1, free / 3));
                rightRows.push_back("----------");
                for (int k = 0; k < thrN; ++k) rightRows.push_back(thr[k]);
                free = rows - static_cast<int>(rightRows.size());
            }
            if (free > 2 && !st.members.empty()) {
                int mn = std::min(static_cast<int>(st.members.size()), free - 1);
                rightRows.push_back("----------");
                for (int k = 0; k < mn; ++k) {
                    rightRows.push_back(memberRowStr(st, st.members[k], true));
                }
            }
        }
    } else if (st.rightPanel == 3) {
        // Threads across ALL rooms.
        for (const auto& r : st.rooms) {
            std::string rid = r.value("room_id", "");
            std::string rname = r.value("name", rid);
            auto evs = st.db->getEvents(rid, 300);
            int shown = 0;
            for (const auto& ev : evs) {
                int rc = 0;
                for (const auto& ev2 : evs) {
                    if (eventThreadRoot(ev2) == ev.event_id) rc++;
                }
                if (rc > 0 && shown < 3) {
                    rightRows.push_back(clip(rname, 10) + ": \u2937 "
                                        + clip(eventBodyImpl(ev), 16) + " (" +
                                        std::to_string(rc) + ")");
                    shown++;
                }
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no threads anywhere)");
    } else if (st.rightPanel == 4) {
        // The agent activity: the standing goal + the last conversation
        // (the auto-saved session). The static --right agent re-reads it.
        std::vector<agenttools::Message> hist;
        agenttools::loadSession(".agent-sessions/last.json", hist);
        agenttools::GoalState goal;
        agenttools::loadGoal(".agent-goal.json", goal);
        {
            // The current model + provider always visible here.
            agenttools::Config acfg;
            agenttools::loadAgentConfig(acfg);
            rightRows.push_back("[1m" + acfg.provider + " / "
                                + acfg.model + "[0m");
        }
        {
            std::ifstream tf(".agent-sessions/last.title");
            if (tf) {
                std::ostringstream tss;
                tss << tf.rdbuf();
                std::string title = tss.str();
                if (!title.empty()) rightRows.push_back(title);
            }
        }
        for (const auto& [stt, content] : agenttools::agentTodos()) {
            rightRows.push_back((stt == "in_progress" ? "→ " :
                                 stt == "completed" ? "✓ " : "· ") + content);
        }
        if (!goal.goal.empty()) {
            rightRows.push_back(clip("goal: " + goal.goal, rightW - 1));
            for (const auto& sg : goal.subgoals) {
                rightRows.push_back(clip("  ▸ " + sg, rightW - 1));
            }
            rightRows.push_back("----------");
        }
        auto pushWrapped = [&](const std::string& line) {
            auto lines = wrapTextImpl(line, std::max(8, rightW - 1));
            for (size_t li = 0; li < lines.size(); ++li) {
                rightRows.push_back((li == 0 ? std::string()
                                             : std::string(2, ' ')) + lines[li]);
            }
        };
        int n = static_cast<int>(hist.size());
        int shown = 0;
        for (int i = std::max(0, n - 24); i < n && shown < 24; ++i, ++shown) {
            const auto& m = hist[static_cast<size_t>(i)];
            if (m.role == "user") {
                pushWrapped("▸ " + m.content);
            } else if (m.role == "assistant") {
                pushWrapped("◂ " + m.content);
            } else if (m.role == "tool") {
                pushWrapped("  · " + m.toolName + " → " + m.content);
            }
        }
        if (rightRows.empty()) rightRows.push_back("(no agent activity yet)");
    }
    // The bottom-right corner: free rows under the right panel content get
    // the recent notifications (pings @me, receipts in 100%-monitored
    // rooms), each with a timestamp. Settings: monitor <room> <0-100|off>
    // (100% = receipts notify) and notifications on|off.
    if (!st.mobile && st.showNotifications) {
        int free = rows - static_cast<int>(rightRows.size());
        if (free >= 2) {
            auto notifClock = [&](const std::tm& tm) {
                char buf[20];
                int h12 = tm.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                const char* ap = tm.tm_hour < 12 ? "AM" : "PM";
                if (st.clock12h) {
                    std::snprintf(buf, sizeof(buf),
                                  st.showSeconds ? "%d:%02d:%02d %s"
                                                 : "%d:%02d %s",
                                  h12, tm.tm_min,
                                  st.showSeconds ? tm.tm_sec : 0, ap);
                } else {
                    std::snprintf(buf, sizeof(buf),
                                  st.showSeconds ? "%02d:%02d:%02d"
                                                 : "%02d:%02d",
                                  tm.tm_hour, tm.tm_min,
                                  st.showSeconds ? tm.tm_sec : 0);
                }
                return std::string(buf);
            };
            auto notifTime = [&](int64_t ts) {
                std::time_t t = static_cast<std::time_t>(ts / 1000)
                              + static_cast<std::time_t>(st.tzOffset) * 3600;
                std::tm tm{};
                localtime_r(&t, &tm);
                std::time_t now = std::time(nullptr);
                std::tm tmNow{};
                localtime_r(&now, &tmNow);
                int64_t dayNow = tmNow.tm_year * 500 + tmNow.tm_yday;
                int64_t day    = tm.tm_year * 500    + tm.tm_yday;
                std::string datePart;
                if (day == dayNow) {
                    datePart = "";
                } else if (day == dayNow - 1) {
                    datePart = "yest ";
                } else if (tm.tm_year == tmNow.tm_year) {
                    static const char* MON[] = {"Jan","Feb","Mar","Apr","May",
                        "Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                    char dbuf[16];
                    std::snprintf(dbuf, sizeof(dbuf), "%s %d ",
                                  MON[tm.tm_mon], tm.tm_mday);
                    datePart = dbuf;
                } else {
                    static const char* MON[] = {"Jan","Feb","Mar","Apr","May",
                        "Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                    char dbuf[16];
                    std::snprintf(dbuf, sizeof(dbuf), "%s %d %02d ",
                                  MON[tm.tm_mon], tm.tm_mday,
                                  (tm.tm_year + 1900) % 100);
                    datePart = dbuf;
                }
                return datePart + notifClock(tm);
            };
            rightRows.push_back(st.showEmoji ? "\xf0\x9f\x94\x94 notifications"
                                             : "[notif]");
            if (st.notifications.empty()) {
                rightRows.push_back(clip(
                    "\x1b[90m(no recent ones — pings @you and receipts "
                    "from rooms monitored at 100% land here)\x1b[0m",
                    std::max(8, rightW - 1)));
            } else {
                int shown = 0;
                bool squeezed = false;  // one row left: previews go inline
                int budget = std::max(8, rightW - 1);
                for (const auto& n : st.notifications) {
                    if (shown >= free - 1) break;
                    std::string marker;
                    switch (n.kind) {
                        case 1:  // ping
                            marker = st.showEmoji
                                 ? "\x1b[1;33m\xf0\x9f\x94\x94\x1b[0m "
                                 : "[ping] ";
                            break;
                        case 2:  // reply to my message
                            marker = st.showEmoji
                                 ? "\x1b[36m\xe2\x86\xaa\x1b[0m "
                                 : "[reply] ";
                            break;
                        case 3:  // my message read
                            marker = st.showEmoji
                                 ? "\x1b[32m\xe2\x9c\x93\x1b[0m "
                                 : "[seen] ";
                            break;
                        default:
                            marker = "  ";
                    }
                    // Split "sender pinged you: <preview>" — the preview
                    // gets its own indented row while rows allow, then
                    // inline so the corner keeps showing older entries.
                    std::string head = n.text, preview;
                    size_t colon = n.text.find(": ");
                    if (colon != std::string::npos) {
                        head = n.text.substr(0, colon);
                        preview = n.text.substr(colon + 2);
                    }
                    bool twoLine = !squeezed && !preview.empty() &&
                                   shown + 2 < free && shown < 8;
                    // The head is never clipped: when the room+time eats
                    // the panel, the date goes first, then the head wraps
                    // below the meta line (never cut a word).
                    std::string ts = notifTime(n.ts);
                    std::string clockOnly = ts;
                    if (!clockOnly.empty() && clockOnly[0] != '0' &&
                        (clockOnly[0] < '0' || clockOnly[0] > '9')) {
                        std::time_t t2 = static_cast<std::time_t>(n.ts / 1000)
                                       + st.tzOffset * 3600;
                        std::tm tm2{};
                        localtime_r(&t2, &tm2);
                        clockOnly = notifClock(tm2);
                    }
                    std::string roomClip =
                        clip(n.room, std::max(3, rightW / 3));
                    std::string meta =
                        "\x1b[90m" + ts + " " + roomClip + "\x1b[0m ";  // date + room
                    std::string metaNoDate =
                        "\x1b[90m" + clockOnly + " "
                        + roomClip + "\x1b[0m ";                       // time + room
                    std::string line = marker + meta + head;
                    bool dropped = false;
                    int headCells = displayWidth(head);
                    int base = displayWidth(marker) + displayWidth(meta);
                    if (base + headCells > budget) {
                        int base2 = displayWidth(marker)
                                  + displayWidth(metaNoDate);
                        if (base2 + headCells <= budget) {
                            line = marker + metaNoDate + head;
                            dropped = true;
                        }
                    }
                    if (dropped || displayWidth(line) <= budget) {
                        int need = 1 + (twoLine ? 1 : 0);
                        if (shown + need > free - 1) break;
                        if (!twoLine && !preview.empty()) {
                            line += " \x1b[90m\xe2\x80\x94 "
                                  + clip(preview, budget - displayWidth(line)) + "\x1b[0m";
                        }
                        rightRows.push_back(clip(line, budget));
                        shown++;
                        if (twoLine) {
                            rightRows.push_back(clip(
                                "    \x1b[90m" + preview + "\x1b[0m", budget));
                            shown++;
                        } else if (!preview.empty()) {
                            squeezed = true;
                        }
                        continue;
                    }
                    // The head is longer than the whole panel: wrap it on
                    // indented continuation rows (never cut a word).
                    std::string ind = "    ";
                    auto headRows = wrapTextImpl(head, std::max(8, budget - 4));
                    int need = 1 + static_cast<int>(headRows.size())
                             + (twoLine ? 1 : 0);
                    if (shown + need > free - 1) break;
                    rightRows.push_back(clip(marker + meta, budget));
                    shown++;
                    for (size_t hi = 0; hi < headRows.size(); ++hi) {
                        std::string hr = ind + headRows[hi];
                        if (hi + 1 == headRows.size() && !twoLine &&
                            !preview.empty()) {
                            hr += " \x1b[90m\xe2\x80\x94 " + preview + "\x1b[0m";
                        }
                        rightRows.push_back(clip(hr, budget));
                        shown++;
                        if (hi + 1 == headRows.size() && twoLine &&
                            !preview.empty()) {
                            rightRows.push_back(clip(
                                ind + ind + "\x1b[90m" + preview + "\x1b[0m",
                                budget));
                            shown++;
                        }
                    }
                    if (!preview.empty() && !twoLine) squeezed = true;
                }
            }
        }
    }
    // Smartphone mode: the rooms list, the chat and the members become one
    // long stacked stream (section separators between them) — the phone
    // scrolls it like a web page.
    if (st.mobile) {
        // Element Classic style: ONE pane at a time — Rooms / Chat / People —
        // chosen by the bottom navigation bar. The pane scrolls on its own.
        std::vector<std::string> stream;
        std::string section;
        if (st.mobileTab == 0) {
            // Invited rooms get their own section at the top, like Element.
            std::vector<std::string> invRows;
            std::vector<std::string> allRows;
            for (const auto* r : visible) {
                std::string rid = r->value("room_id", "");
                std::string mark = rid == st.currentRoomId ? "*" : " ";
                std::string name = roomDisplayNameImpl(*r);
                if (r->value("is_direct", false))
                    name = (st.showEmoji ? "💬 " : "[DM] ") + name;
                if (st.invited.count(rid)) {
                    mark += (st.showEmoji ? "📨 " : "[INV] ");
                }
                std::vector<std::string>& dst = st.invited.count(rid) ? invRows : allRows;
                std::string row = mark + "[1m" + name + "[0m ("
                                + std::to_string(roomMessageCount(st.db, rid)) + ")";
                if (r->value("is_encrypted", false)) {
                    row += (st.showEmoji ? " 🔒" : " [E2EE]");
                }
                if (st.mutedRooms.count(rid)) {
                    row += (st.showEmoji ? " 🔇" : " [muted]");
                }
                if (st.starredRooms.count(rid)) {
                    row += " ★";  // ★
                }
                // The room description (topic) in dim, after the count.
                {
                    std::string topic = r->value("topic", "");
                    if (!topic.empty()) {
                        int used = displayWidth(row) + 2;
                        row += "[90m" + clip(" · " + topic,
                                                 std::max(4, W - 8 - used))
                             + "[0m";
                    }
                }
                std::string ltime = roomLastTime(st.db, rid, st.showSeconds,
                                                   st.clock12h);
                // For invited rooms the time slot shows the remembered
                // invitation date instead of the last message time.
                {
                    auto invIt = st.inviteByRoom.find(rid);
                    if (invIt != st.inviteByRoom.end() && invIt->second.ts > 0) {
                        ltime = "invited " + relativeTime(invIt->second.ts);
                        if (st.db->getSetting("invreason_menu", "0") != "0" &&
                            !invIt->second.reason.empty()) {
                            ltime += " — " + invIt->second.reason;
                        }
                    }
                }
                if (!ltime.empty()) {
                    int tl = displayWidth(ltime);
                    int baseW = W - tl - 1;
                    row = clip(row, baseW);
                    row += std::string(std::max(1, W - baseW - tl), ' ')
                         + "[90m" + ltime + "[0m";
                }
                int thr = st.db->getSetting("threads_off", "0") == "0"
                              ? roomThreadCount(st.db, rid) : 0;
                if (thr > 0) {
                    row += (st.showEmoji ? " 🧵" : " (threads ")
                         + std::to_string(thr) + (st.showEmoji ? "" : ")");
                }
                dst.push_back(row);
                std::string last = roomLastMsg(st.db, rid, st.rooms);
                if (!last.empty()) {
                    auto colon = last.find(':');
                    std::string who = colon == std::string::npos
                                          ? last : last.substr(0, colon + 1);
                    std::string what = colon == std::string::npos
                                           ? "" : last.substr(colon + 1);
                    dst.push_back("  " + who + "[90m"
                                + highlightMentions(
                                      clip(what,
                                           std::max(2, W - 3 - displayWidth(who))))
                                + "[0m");
                }
            }
            if (!invRows.empty()) {
                stream.push_back("📨 Invites");
                stream.insert(stream.end(), invRows.begin(), invRows.end());
                stream.push_back("-- Rooms --");
            }
            stream.insert(stream.end(), allRows.begin(), allRows.end());
            section = " Rooms ";
        } else if (st.mobileTab == 1) {
            stream.push_back(clip("── " + roomName + " ──", std::max(1, W - 1)));
            stream.insert(stream.end(), centerRows.begin(), centerRows.end());
            section = " Chat ";
        } else {
            stream.insert(stream.end(), rightRows.begin(), rightRows.end());
            section = " People ";
        }
        int total = static_cast<int>(stream.size());
        int maxScroll = std::max(0, total - rows);
        scroll = std::min(scroll, maxScroll);
        if (scroll > 0) out += "  ^ more above (scroll up)\n";
        if (scroll + rows < total) out += "  v more below (scroll down)\n";
        for (int i = 0; i < rows; ++i) {
            int src = scroll + i;
            out += (src < total) ? clip(stream[static_cast<size_t>(src)], std::max(1, W - 1)) + "\n"
                                 : "\n";
        }
        out += repeat('=', std::max(0, W - 1)) + "\n";
        // Bottom navigation (Element Classic style): the active tab is
        // bracketed, the others are bare.
        std::string nav;
        for (int t = 0; t < 3; ++t) {
            const char* label = (t == 0) ? "Rooms" : (t == 1) ? "Chat" : "People";
            int padn = (W - 18) / 2 - 2;  // ~centered
            if (t == st.mobileTab) {
                nav += "\x1b[7m" + std::string(label) + "\x1b[0m";
            } else {
                nav += std::string(label);
            }
            if (t < 2) nav += "  ";
        }
        std::string padnav = std::string(std::max(0, (W - 20) / 2), ' ') + nav;
        out += clip(padnav, W) + "\n";
        std::string pos;
        if (total > rows) {
            pos = " [rows " + std::to_string(scroll + 1) + "-"
                + std::to_string(scroll + rows) + " of "
                + std::to_string(total) + "]";
        }
        out += "proxy: " + st.proxyLabel + " | scroll: up/down/top/bottom" + pos;
        if (!st.statusNote.empty()) out += " | " + st.statusNote;
        out += "\n";
        out += "rooms | chat | people | open <room> | send <room> <text> |"
               " find <q> | search <q> | settings | help | quit\n";
        return out;
    }

    // Clamp the scroll into [0, contentRows - rows] (state stays const).
    int visCount = st.roomFilter.empty() ? contentRowsImpl(st)
                                         : static_cast<int>(visible.size());
    visCount = std::max(visCount, static_cast<int>(centerRows.size()));
    visCount = std::max(visCount, static_cast<int>(rightRows.size()));
    int maxScroll = std::max(0, visCount - rows);
    scroll = std::min(scroll, maxScroll);
    // ---- The left panel, built once per frame (Element-style): invited
    // rooms first (a "📨 Invites" header), then each space as its own
    // section ("▸ Tech Space (n)" — sized to how many rooms it needs),
    // then the rooms with no space as the second part. Every row uses
    // the full panel: the preview is clipped and the last-message time
    // is flushed right against the pipe.
    std::vector<std::string> leftRows;
    std::vector<std::string> bucketName;   // the spaces, first seen order
    std::unordered_map<std::string, int> spaceIdx;
    for (const auto& r : st.rooms) {
        if (!r.value("is_space", false)) continue;
        std::string sid = r.value("room_id", "");
        if (!spaceIdx.count(sid)) {
            spaceIdx[sid] = static_cast<int>(bucketName.size());
            bucketName.push_back(r.value("name", "?"));
        }
    }
    const int noSpaceBucket = static_cast<int>(bucketName.size());
    std::vector<std::vector<std::string> > bucketRows(bucketName.size() + 1);
    std::vector<std::string> invRows;
    for (const auto* r : visible) {
        std::string rid = r->value("room_id", "");
        bool invited = st.invited.count(rid) != 0;
        std::string name = roomDisplayNameImpl(*r);
        if (r->value("is_direct", false))
            name = (st.showEmoji ? "💬 " : "[DM] ") + name;
        std::string head = std::string(1, rid == st.currentRoomId ? '*' : ' ')
                         + "\x1b[1m" + name + "\x1b[0m ("
                         + std::to_string(roomMessageCount(st.db, rid)) + ")";
        if (r->value("is_encrypted", false))
            head += st.showEmoji ? " 🔒" : " [E2EE]";
        if (st.mutedRooms.count(rid)) head += st.showEmoji ? " 🔇" : " [muted]";
        if (st.starredRooms.count(rid)) head += " ★";
        if (st.db->getSetting("threads_off", "0") == "0") {
            int thr = roomThreadCount(st.db, rid);
            if (thr > 0)
                head += (st.showEmoji ? " 🧵" : " (threads ") + std::to_string(thr)
                      + (st.showEmoji ? "" : ")");
        }
        std::string tail;  // the right-flushed suffix ("(invited …)" or the time)
        if (invited && st.showRoomInviteMark) {
            auto invIt = st.inviteByRoom.find(rid);
            std::string when;
            if (invIt != st.inviteByRoom.end() && invIt->second.ts > 0)
                when = "invited " + relativeTime(invIt->second.ts);
            tail = (st.showEmoji ? "📨 (" : "(") + when + ")";
        } else if (!invited) {
            std::string last = roomLastMsg(st.db, rid, st.rooms);
            if (!last.empty()) {
                auto colon = last.find(':');
                std::string who = colon == std::string::npos
                                      ? last : last.substr(0, colon + 1);
                std::string what = colon == std::string::npos
                                       ? "" : last.substr(colon + 1);
                std::string prev = " · " + who + what;
                std::string ltime = roomLastTime(st.db, rid, st.showSeconds,
                                                 st.clock12h);
                if (!ltime.empty()) {
                    tail = ltime;
                    // The preview fills whatever the name, icons and the
                    // right-flushed time leave free (clipped, never
                    // rejected because of its own length).
                    int budget = leftW - displayWidth(head)
                               - displayWidth(ltime) - 8;
                    if (budget >= 6)
                        head += " \x1b[90m"
                              + highlightMentions(clip(prev, budget)) + "\x1b[0m";
                }
            }
        }
        std::string row = head;
        if (!tail.empty()) {
            int slack = leftW - displayWidth(row) - displayWidth(tail) - 1;
            if (slack > 0) row += std::string(slack, ' ') + "\x1b[90m" + tail + "\x1b[0m";
            else row += " \x1b[90m" + tail + "\x1b[0m";
        }
        row = clip(row, leftW - 1);
        if (invited) {
            invRows.push_back(row);
        } else {
            std::string sp = r->value("space", "");
            auto si = spaceIdx.find(sp);
            int b = si != spaceIdx.end() ? si->second : noSpaceBucket;
            bucketRows[static_cast<size_t>(b)].push_back(row);
        }
    }
    if (!invRows.empty()) {
        leftRows.push_back(" \x1b[1m📨 Invites\x1b[0m");
        leftRows.insert(leftRows.end(), invRows.begin(), invRows.end());
    }
    bool anySpace = false;
    for (size_t b = 0; b < bucketName.size(); ++b) {
        if (bucketRows[b].empty()) continue;
        anySpace = true;
        leftRows.push_back("  \x1b[1m▸ " + bucketName[b] + "\x1b[0m \x1b[90m("
                         + std::to_string(bucketRows[b].size()) + ")\x1b[0m");
        leftRows.insert(leftRows.end(), bucketRows[b].begin(), bucketRows[b].end());
    }
    if (!bucketRows[static_cast<size_t>(noSpaceBucket)].empty()) {
        if (anySpace)
            leftRows.push_back("  \x1b[1m-- No space (" + std::to_string(bucketRows[static_cast<size_t>(noSpaceBucket)].size()) + ") --\x1b[0m");
        leftRows.insert(leftRows.end(),
                        bucketRows[static_cast<size_t>(noSpaceBucket)].begin(),
                        bucketRows[static_cast<size_t>(noSpaceBucket)].end());
    }
    // The rooms list can scroll on its own (--scroll-left): only the
    // left panel moves, the chat and members stay put.
    int leftScroll = std::min(std::max(0, st.leftScroll),
                              std::max(0, static_cast<int>(leftRows.size()) - rows));
    if (scroll > 0) out += "  ^ more above (scroll up)\n";
    if (scroll + rows < contentRowsImpl(st)) out += "  v more below (scroll down)\n";
    for (int i = 0; i < rows; ++i) {
        int src = scroll + i;  // the content row this view row shows
        int leftSrc = leftScroll + i;  // the rooms row (scrolled separately)
        std::string left, center, right;
        if (leftSrc < static_cast<int>(leftRows.size())) {
            left = leftRows[static_cast<size_t>(leftSrc)];
        }
        if (src < static_cast<int>(centerRows.size())) {
            center = centerRows[static_cast<size_t>(src)];
        }
        if (src < static_cast<int>(rightRows.size())) {
            right = rightRows[static_cast<size_t>(src)];
        }
        std::string rightOut = clip(right, static_cast<size_t>(rightW));
        if (rightOut.find('\x1b') != std::string::npos) rightOut += "\x1b[0m";
        // The right panel gets no trailing padding: a row of exactly W
        // cells makes Konsole wrap onto a phantom blank line.
        out += pad(left, static_cast<size_t>(leftW)) + PIPE + "|" + X
             + pad(center, static_cast<size_t>(centerW)) + PIPE + "|" + X
             + rightOut + "\n";
    }

    // Status line
    out += repeat('=', std::max(0, W - 1)) + "\n";
    std::string pos;
    if (contentRowsImpl(st) > rows) {
        pos = " [rows " + std::to_string(scroll + 1) + "-"
            + std::to_string(scroll + rows) + " of "
            + std::to_string(contentRowsImpl(st)) + "]";
    }
    out += "proxy: " + st.proxyLabel + " | scroll: up/down/top/bottom" + pos;
    if (!st.statusNote.empty()) {
        // The last action's summary lives in the RIGHT-BOTTOM corner.
        out += " | " + st.statusNote;
    }
    out += "\n";
    out += "open <room> | send <room> <text> | find <q> | search <q> | thread |"
           " dump <room> | settings | space | help | quit\n";
    return out;
}

} // namespace matrixcli

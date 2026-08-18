// src/ascii_ui_draw_center.cpp — the chat panel's rows: the message
// bodies, votes, polls, reactions, date separators and the wrap/time
// formatting. Split out of ascii_ui_draw.cpp (the “1k lines per
// file” rule) so both translation units stay under 1000 lines.
#include "ascii_ui_impl.hpp"
#include "ascii_state.hpp"
#include "commands.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/string_utils.hpp"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>
#include <map>

namespace matrixcli {

std::vector<std::string> buildCenterRows(const UiState& st, int centerW,
                                         int W, bool horizMembers) {
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
                        // The voting's question, inline grey on the SAME
                        // line as the vote (clipped to the row's width).
                        auto q = pev.content.find("question");
                        if (q != pev.content.end() && q->is_object()) {
                            std::string qtext = q->value("text", "");
                            if (!qtext.empty()) {
                                // The question fills the rest of the same line (never a
                                // line of its own).
                                int budget = (st.mobile ? W : centerW) - 1 - 6
                                           - displayWidth(center) - 4;
                                if (budget >= 8) {
                                    center += "  \x1b[90m("
                                            + clip(qtext, budget) + ")\x1b[0m";
                                }
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
                // The thread hint sits on the SAME line as the message
                // (grey, no author — the sender is already in the row),
                // clipped to the remaining panel width.
                std::string preview;
                matrix::Event rootEv;
                if (st.db && st.db->getEventById(thr, rootEv))
                    preview = eventBodyImpl(rootEv);
                if (preview.empty()) preview = thr;
                std::string head = "[" + chatName(st, st.currentRoomId, ev.sender)
                                 + "] \u2937 " + body;
                // The hint sits on the SAME line as the message and fills it
                // edge to edge: budget = first line (time + border) minus
                // the head and the "  (thread: " + ")" wrappers. Missing
                // the space means no hint at all.
                int budget = (st.mobile ? W : centerW) - 1 - 6
                           - displayWidth(head) - 12;
                if (budget >= 8) {
                    center = head + "  \x1b[90m(thread: "
                           + clip(preview, budget) + ")\x1b[0m";
                } else {
                    center = head;
                }
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
                    // Inline when it fits ("23:10 [alice] ⭕ poll: … 🗳 …");
                    // otherwise the counts go on their own dim line so the
                    // question text never wraps mid-word.
                    int effW = (st.mobile ? W : centerW) - 8;
                    int used = 6 + static_cast<int>(center.size()) + 4
                             + static_cast<int>(vstr.size());
                    if (used <= effW)
                        center += "  \x1b[32m\xf0\x9f\x97\xb3 " + vstr + "\x1b[0m";  // 🗳
                    else
                        center += "\n  \x1b[32m\xf0\x9f\x97\xb3 " + vstr + "\x1b[0m";
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
                // The day's message rate: count / the hours elapsed in
                // that day (a partial day counts only its elapsed part).
                double hours = 24.0;
                {
                    int64_t dayStart = static_cast<int64_t>(day) * 86400000LL;
                    int64_t dayEnd = dayStart + 86400000LL;
                    int64_t nowMs = static_cast<int64_t>(std::time(nullptr)) * 1000LL;
                    hours = static_cast<double>(
                        (std::min(nowMs, dayEnd) - dayStart)) / 3600000.0;
                    if (hours <= 0.05) hours = 24.0;
                }
                std::string sep = "── " + label + " ──  " + std::to_string(dayCount)
                                + " msgs";
                char rateBuf[24];
                std::snprintf(rateBuf, sizeof(rateBuf), " (%.1f/hr)",
                              dayCount / hours);
                sep += rateBuf;
                if (static_cast<int>(sep.size()) < centerW) {
                    sep = std::string((centerW - static_cast<int>(sep.size())) / 2, ' ') + sep;
                }
                if (st.db->getSetting("date_sep", "1") != "0") {
                    centerRows.push_back("\x1b[90m" + sep + "\x1b[0m");
                }
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
                    // 'receipts hide <user>' — their ✓ reader drops out
                    // of the list (matched by the short name).
                    if (!st.hiddenReceiptUsers.empty()) {
                        std::string filtered;
                        std::istringstream frs(rd);
                        std::string fw;
                        while (frs >> fw) {
                            bool hide = false;
                            for (const auto& h : st.hiddenReceiptUsers) {
                                if (h == fw) { hide = true; break; }
                            }
                            if (!hide) {
                                if (!filtered.empty()) filtered += " ";
                                filtered += fw;
                            }
                        }
                        rd = filtered;
                    }
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
                    if (!shown.empty())
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
                    auto tLines = wrapTextImpl(
                        inner, std::max(8, wrapW - reserve),
                        (st.mobile ? W : centerW) - 1 - reserve);
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
                    std::vector<std::string> lines = wrapTextImpl(
                        first, wrapW, (st.mobile ? W : centerW) - 1);
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
    return centerRows;
}

} // namespace matrixcli

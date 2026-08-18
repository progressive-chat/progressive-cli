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

// The "----------" section rule inside the right panel (threads,
// members, goal, notifications): grey, and gone when the user turned
// the separators off (sep on|off).
static std::string panelSepRule(const matrixcli::UiState& st) {
    if (!st.db || st.db->getSetting("date_sep", "1") == "0") return "";
    return "\x1b[90m----------\x1b[0m";
}

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
    std::vector<std::string> centerRows =
        buildCenterRows(st, centerW, W, horizMembers);
    // Right panel content per mode: members | room thread list | one thread
    // | threads across all rooms (Element-style).
    std::vector<std::string> rightRows;
    if (st.rightPanel == 0) {
        // Full @user:server ids per row, whenever the row fits the panel.
        for (const auto& mem : st.members) {
            rightRows.push_back(memberRowStr(
                st, mem, rightW - 2 >= displayWidth(memberRowStr(st, mem, true))));
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
                std::string psep = panelSepRule(st);
                if (!psep.empty()) rightRows.push_back(psep);
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
                std::string psep = panelSepRule(st);
                if (!psep.empty()) rightRows.push_back(psep);
                for (int k = 0; k < thrN; ++k) rightRows.push_back(thr[k]);
                free = rows - static_cast<int>(rightRows.size());
            }
            if (free > 2 && !st.members.empty()) {
                int mn = std::min(static_cast<int>(st.members.size()), free - 1);
                std::string psep = panelSepRule(st);
                if (!psep.empty()) rightRows.push_back(psep);
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
            std::string psep = panelSepRule(st);
                if (!psep.empty()) rightRows.push_back(psep);
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
            if (st.db->getSetting("date_sep", "1") != "0") {
                stream.push_back("\x1b[90m── " + roomName + " ──\x1b[0m");
            }
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
    std::vector<std::string> bucketSid;    // their room ids (for the people counts)
    std::unordered_map<std::string, int> spaceIdx;
    for (const auto& r : st.rooms) {
        if (!r.value("is_space", false)) continue;
        std::string sid = r.value("room_id", "");
        if (!spaceIdx.count(sid)) {
            spaceIdx[sid] = static_cast<int>(bucketName.size());
            bucketName.push_back(r.value("name", "?"));
            bucketSid.push_back(sid);
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
            if (invIt != st.inviteByRoom.end() && invIt->second.ts > 0) {
                when = "invited " + relativeTime(invIt->second.ts);
                // Who sent the invite: the member event's sender, shown
                // by its localpart ("invited 2h ago by alice").
                if (!invIt->second.inviter.empty()) {
                    std::string by = invIt->second.inviter;
                    size_t at = by.rfind(':');
                    if (at != std::string::npos) by = by.substr(1, at - 1);
                    else if (!by.empty() && by[0] == '@') by = by.substr(1);
                    when += " by " + by;
                    // The inviter's role IN THE INVITED ROOM: owner /
                    // admin / moderator / custom powerlevel (n) / normal
                    // user — the v12 creator rule applies too.
                    std::string role;
                    int lvl = 0;
                    auto evs = st.db->getEvents(rid, 300);
                    for (const auto& ev : evs) {
                        if (ev.type != "m.room.power_levels" ||
                            !ev.content.is_object()) continue;
                        auto users = ev.content.find("users");
                        if (users != ev.content.end() && users->is_object()) {
                            auto it = users->find(invIt->second.inviter);
                            if (it != users->end() && it->is_number())
                                lvl = std::max(lvl, it->get<int>());
                        }
                        break;
                    }
                    for (const auto& r2 : st.rooms) {
                        if (r2.value("room_id", "") != rid) continue;
                        if (r2.value("version", 0) >= 12 && lvl < 150) {
                            std::string cr = r2.value("creator", "");
                            size_t ca = cr.rfind(':');
                            std::string crLocal = ca != std::string::npos
                                                      ? cr.substr(0, ca) : cr;
                            if (!crLocal.empty() && crLocal[0] == '@')
                                crLocal = crLocal.substr(1);
                            if (crLocal == by) lvl = 150;
                        }
                        break;
                    }
                    if (lvl >= 150) role = "owner";
                    else if (lvl >= 100) role = "admin";
                    else if (lvl >= 50) role = "moderator";
                    else if (lvl >= 1)
                        role = "custom powerlevel (" + std::to_string(lvl) + ")";
                    else role = "normal user";
                    when += ", " + role;
                    // The openness sign: whether the room is open (public
                    // — anyone can preview/join) or closed (invite-only /
                    // restricted / knock).
                    std::string rule = roomJoinRule(st.db, rid);
                    if (rule == "public") when += ", open";
                    else if (rule == "invite") when += ", invite-only";
                    else if (rule == "restricted") when += ", restricted";
                    else if (rule == "knock") when += ", knock";
                }
            }
            tail = (st.showEmoji ? "📨 (" : "(") + when + ")";
        } else if (!invited) {
            std::string last = roomLastMsg(st.db, rid, st.rooms);
            if (!last.empty()) {
                auto colon = last.find(':');
                std::string who = colon == std::string::npos
                                      ? last : last.substr(0, colon + 1);
                std::string what = colon == std::string::npos
                                       ? "" : last.substr(colon + 1);
                std::string ltime = roomLastTime(st.db, rid, st.showSeconds,
                                                 st.clock12h);
                if (!ltime.empty()) {
                    tail = ltime;
                    // The preview fills the free columns exactly (one
                    // space stays before the time): the "·" separator is
                    // dim, the nickname keeps the normal color and only
                    // the message text is dimmed (like before).
                    int budget = leftW - displayWidth(head)
                               - displayWidth(ltime) - 1;
                    if (budget >= displayWidth(who) + 6)
                        head += " \x1b[90m· \x1b[0m" + who + "\x1b[90m"
                              + highlightMentions(clip(
                                    what,
                                    std::max(2, budget
                                                  - displayWidth(who) - 3)))
                              + "\x1b[0m";
                }
            }
        }
        std::string row = head;
        if (!tail.empty()) {
            // The time sits at the very last column — the pipe right
            // after it, no trailing padding (the left panel is never
            // the full terminal width, so there is no wrap artifact).
            int slack = leftW - displayWidth(row) - displayWidth(tail);
            if (slack > 0) row += std::string(slack, ' ') + "\x1b[90m" + tail + "\x1b[0m";
            else row += " \x1b[90m" + tail + "\x1b[0m";
        }
        // A safety net for the (rare) over-long head; the paint pads
        // the row to the panel width, so nothing ever spills past it.
        row = clip(row, leftW);
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
        // People from all rooms: the members across THIS space's rooms;
        // "in space": the space's own members — both on the space line.
        int allPeople = 0, spacePeople = 0;
        for (const auto& r : st.rooms) {
            if (r.value("is_space", false)) {
                if (r.value("room_id", "") == bucketSid[b])
                    spacePeople = r.value("member_count", 0);
            } else if (r.value("space", "") == bucketSid[b]) {
                allPeople += r.value("member_count", 0);
            }
        }
        leftRows.push_back("\x1b[1m▸ " + bucketName[b] + "\x1b[0m \x1b[90m("
                         + std::to_string(bucketRows[b].size()) + ") "
                         + "People from all rooms: " + std::to_string(allPeople)
                         + ", in space: " + std::to_string(spacePeople)
                         + "\x1b[0m");
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

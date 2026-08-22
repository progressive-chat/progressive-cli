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

#ifdef _WIN32
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

#include "ascii_ui_impl.hpp"

namespace matrixcli {
PanelRule parsePanelRule(const std::string& spec) {
    PanelRule r;
    if (spec.empty() || spec == "off") return r;
    auto colon = spec.find(':');
    std::string k = colon == std::string::npos ? spec : spec.substr(0, colon);
    std::string v = colon == std::string::npos ? "" : spec.substr(colon + 1);
    int val = 0;
    try { val = std::stoi(v); } catch (...) { return r; }
    if (val <= 0) return r;
    if (k == "min") { r.kind = 1; r.val = val; }
    else if (k == "max") { r.kind = 2; r.val = val; }
    else if (k == "pct") { r.kind = 3; r.val = std::min(100, val); }
    return r;
}

std::string panelRuleText(const PanelRule& r) {
    if (r.kind == 1) return "min " + std::to_string(r.val);
    if (r.kind == 2) return "max " + std::to_string(r.val);
    if (r.kind == 3) return std::to_string(r.val) + "%";
    return "off";
}

PanelLayout computePanelLayout(const UiState& st, int W, bool trace) {
    PanelLayout L;
    auto note = [&](const std::string& s) {
        if (trace) L.notes.push_back(s);
    };
    // Few members: the user list goes horizontal (one row across the top
    // of the chat) and the right panel is freed - auto mode, or forced.
    bool horizMembers = !st.mobile && st.rightPanel == 0 &&
        (st.membersMode == 1 ||
         (st.membersMode == 0 && st.members.size() <= 4));
    note("terminal width W = " + std::to_string(W) +
         (st.termW > 0 ? " (from the render request)" : " (ioctl / COLUMNS)"));
    int leftW = st.leftPanelW >= 0 ? st.leftPanelW : std::max(22, W / 5);
    int rightW = st.rightPanelW >= 0 ? st.rightPanelW : std::max(16, W / 6);
    if (st.leftPanelW == 0) leftW = 0;
    if (st.rightPanelW == 0) rightW = 0;
    if (st.leftPanelW < 0)
        note("left: default max(22, W/5) = " + std::to_string(leftW));
    else if (st.leftPanelW == 0)
        note("left: hidden (panel left off)");
    else
        note("left: fixed width " + std::to_string(leftW) + " (panel left "
             + std::to_string(st.leftPanelW) + ")");
    if (st.rightPanelW < 0)
        note("right: default max(16, W/6) = " + std::to_string(rightW));
    else if (st.rightPanelW == 0)
        note("right: hidden (panel right off)");
    else
        note("right: fixed width " + std::to_string(rightW) + " (panel right "
             + std::to_string(st.rightPanelW) + ")");
    if (st.autoPanels) {
        // Size the panels to the content so the screen is filled: the
        // room panel fits the longest room row plus a preview budget, the
        // member panel fits the longest member name.
        // The rooms list is sized to the LONGEST ACTUAL row (name, count,
        // thread marker and the last-message preview), so no column space
        // is wasted — the chat keeps the rest.
        int longestRoom = 0;
        for (const auto& r : st.rooms) {
            if (r.value("is_space", false)) continue;
            std::string rid = r.value("room_id", "");
            std::string nm = roomDisplayNameImpl(st, r);
            if (r.value("is_direct", false)) nm = "  " + nm;
            int w = displayWidth(" " + nm + " ("
                                 + std::to_string(roomMessageCount(st.db, rid)) + ")");
            auto invIt = st.inviteByRoom.find(rid);
            if (invIt != st.inviteByRoom.end() && invIt->second.ts > 0) {
                // Invited rooms render "📨 <when>" instead of the
                // last-message preview.
                w += displayWidth(" 📨 ") + displayWidth(relativeTime(invIt->second.ts));
            } else {
                std::string last = roomLastMsg(st.db, rid, st.rooms);
                if (!last.empty()) w += 3 + std::min(44, displayWidth(last));  // " · preview"
            }
            if (st.db->getSetting("threads_off", "0") == "0" &&
                roomThreadCount(st.db, rid) > 0) w += 4;  // " 🧵N"
            if (w > longestRoom) longestRoom = w;
        }
        // The section headers count too: "▸ Tech Space (21) People from
        // all rooms: 1022, in space: 312" is longer than any room row, and
        // the list must never clip them while the chat keeps idle space.
        // (The preview text is measured to a fixed 44-cell budget — the
        // rows clip it anyway.)
        {
            std::vector<std::string> bucketName;
            std::vector<std::string> bucketSid;
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
            std::vector<int> rowN(bucketName.size() + 1, 0);
            for (const auto& r : st.rooms) {
                if (r.value("is_space", false)) continue;
                auto si = spaceIdx.find(r.value("space", ""));
                ++rowN[si != spaceIdx.end() ? si->second : noSpaceBucket];
            }
            if (static_cast<int>(st.invited.size()) > 0)
                longestRoom = std::max(longestRoom,
                                       displayWidth(" 📨 Invites"));
            for (size_t b = 0; b < bucketName.size(); ++b) {
                if (rowN[b] == 0) continue;
                int allPeople = 0, spacePeople = 0;
                for (const auto& r : st.rooms) {
                    if (r.value("is_space", false)) {
                        if (r.value("room_id", "") == bucketSid[b])
                            spacePeople = r.value("member_count", 0);
                    } else if (r.value("space", "") == bucketSid[b]) {
                        allPeople += r.value("member_count", 0);
                    }
                }
                int w = displayWidth("▸ " + bucketName[b] + " ("
                                     + std::to_string(rowN[b]) + ") "
                                     + "People from all rooms: "
                                     + std::to_string(allPeople)
                                     + ", in space: "
                                     + std::to_string(spacePeople)) + 2;
                if (w > longestRoom) longestRoom = w;
            }
            if (rowN[static_cast<size_t>(noSpaceBucket)] > 0) {
                longestRoom = std::max(
                    longestRoom,
                    displayWidth("-- No space ("
                                 + std::to_string(rowN[static_cast<size_t>(noSpaceBucket)])
                                 + ") --"));
            }
        }
        leftW = std::max(24, std::min(72, longestRoom + 2));
        note("left: auto — the longest room row is " + std::to_string(longestRoom)
             + " cells, width = clamp(longest + 2, 24..72) = "
             + std::to_string(leftW));
        if (horizMembers) {
            rightW = 0;
            note("right: the members row is horizontal (few members / members "
                 "auto) — the panel is freed");
        } else {
            int longestMember = 0;
            int fullMember = 0;
            for (const auto& mem : st.members) {
                int w = displayWidth(memberRowStr(st, mem));
                if (w > longestMember) longestMember = w;
                int wf = displayWidth(memberRowStr(st, mem, true));
                if (wf > fullMember) fullMember = wf;
            }
            // The full matrix ids fit when they stay inside the clamp.
            int rightMax = std::min(40, std::max(24, W / 3));
            // The full matrix ids (mallory @matrix.org, wendy @mozilla.org)
            // count too — whenever the panel cap covers them (the +3 pad
            // is clipped by the cap) the list shows them instead of the
            // short localparts.
            if (fullMember <= rightMax) longestMember = std::max(longestMember, fullMember);
            rightW = std::max(10, std::min(rightMax, longestMember + 3));
            note("right: auto — the longest member row is "
                 + std::to_string(longestMember) + " cells, cap = min(40, "
                 "max(24, W/3)) = " + std::to_string(rightMax) + ", width = "
                 + std::to_string(rightW));
        }
    }
    // The user's width rules ("panel rule …") override the base widths.
    PanelRule rl = parsePanelRule(st.ruleLeft);
    if (rl.kind == 3) leftW = W * rl.val / 100;
    else if (rl.kind == 1) leftW = std::max(leftW, rl.val);
    else if (rl.kind == 2) leftW = std::min(leftW, rl.val);
    if (rl.kind) note("left: rule " + panelRuleText(rl) + " -> "
                      + std::to_string(leftW));
    PanelRule rr = parsePanelRule(st.ruleRight);
    if (rr.kind == 3) rightW = W * rr.val / 100;
    else if (rr.kind == 1) rightW = std::max(rightW, rr.val);
    else if (rr.kind == 2) rightW = std::min(rightW, rr.val);
    if (rr.kind) note("right: rule " + panelRuleText(rr) + " -> "
                      + std::to_string(rightW));
    PanelRule rc = parsePanelRule(st.ruleCenter);
    // Keep the chat usable: the panels never squeeze the center below
    // ~30 columns — the rooms list gives way first, then the members.
    // A "center min N" rule raises that floor.
    int minCenter = 30;
    if (rc.kind == 1) minCenter = std::max(minCenter, rc.val);
    if (W - leftW - rightW - 2 < minCenter) {
        leftW = std::max(24, W - rightW - 2 - minCenter);
        note("left squeezed to " + std::to_string(leftW)
             + " to keep the center >= " + std::to_string(minCenter));
    }
    if (W - leftW - rightW - 2 < minCenter) {
        rightW = std::max(10, W - leftW - 2 - minCenter);
        note("right squeezed to " + std::to_string(rightW)
             + " to keep the center >= " + std::to_string(minCenter));
    }
    int centerW = std::max(20, W - leftW - rightW - 2);
    // "center N%" / "center min N": grow the center to the target by
    // squeezing the left panel (down to 24), then the right (down to 10).
    if (rc.kind == 3 || rc.kind == 1) {
        int target = rc.kind == 3 ? W * rc.val / 100 : rc.val;
        if (centerW < target) {
            int need = target - centerW;
            int give = std::min(need, std::max(0, leftW - 24));
            leftW -= give; centerW += give; need -= give;
            int give2 = std::min(need, std::max(0, rightW - 10));
            rightW -= give2; centerW += give2; need -= give2;
            if (need > 0)
                note("center rule " + panelRuleText(rc) + " cannot be fully "
                     "satisfied at W = " + std::to_string(W) + " (short by "
                     + std::to_string(need) + ")");
            else
                note("left/right squeezed to give the center "
                     + std::to_string(target));
        }
        if (rc.kind == 3 && centerW > target) centerW = target;
        note("center: rule " + panelRuleText(rc) + " -> "
             + std::to_string(centerW));
    } else if (rc.kind == 2 && centerW > rc.val) {
        centerW = rc.val;
        note("center: rule max " + std::to_string(rc.val) + " -> "
             + std::to_string(centerW));
    }
    // The chat caps its width: on very wide terminals the leftover stays
    // as a right margin instead of an empty strip inside the message area.
    // An explicit "center N%" rule replaces the default cap.
    int centerCap = st.mobile ? W - 2 : 120;
    if (rc.kind != 3 && centerW > centerCap) {
        centerW = centerCap;
        note("center: capped at " + std::to_string(centerCap)
             + (st.mobile ? " (mobile: W - 2)" : " (the default 120 cap)")
             + " — the rest stays as a right margin");
    }
    note("result: left " + std::to_string(leftW) + " | center "
         + std::to_string(centerW) + " | right " + std::to_string(rightW)
         + "  (+2 pipe columns = "
         + std::to_string(leftW + centerW + rightW + 2) + " of "
         + std::to_string(W) + ")");
    L.leftW = leftW; L.rightW = rightW; L.centerW = centerW;
    L.centerCap = centerCap; L.horizMembers = horizMembers;
    return L;
}

std::string drawFrameImpl(const UiState& st) {
    int W = st.termW > 0 ? st.termW : terminalWidthImpl();
    PanelLayout L = computePanelLayout(st, W, false);
    bool horizMembers = L.horizMembers;
    int leftW = L.leftW, rightW = L.rightW, centerW = L.centerW;

    std::string roomName = "No room selected";
    std::string e2eeMark;  // the lock for the open room
    for (const auto& r : st.rooms) {
        if (r.value("room_id", "") == st.currentRoomId) {
            roomName = roomDisplayNameImpl(st, r);
            if (r.value("is_encrypted", false)) {
                e2eeMark = (st.showEmoji ? " 🔒 " : " [E2EE] ");
            }
            break;
        }
    }

    // Header
    std::string out;
    // The unread count since the last-read marker — the Element-style
    // "· N new" hint in the room header (visible regardless of the
    // viewport scroll).
    int newSinceRead = 0;
    if (!st.readMarker.empty()) {
        bool past = false;
        for (const auto& ev : st.messages) {
            if (!past) {
                if (ev.event_id == st.readMarker) past = true;
                continue;
            }
            if (ev.type == "m.room.message" || ev.type == "m.sticker")
                newSinceRead++;
        }
    }
    std::string header = " " + roomName + e2eeMark;
    if (newSinceRead > 0) header += " · " + std::to_string(newSinceRead) + " new";
    header += " ";
    int headerFill = W - static_cast<int>(header.size()) - 1;
    if (headerFill < 0) headerFill = 0;
    out += header + repeat('=', headerFill) + "\n";

    // Panel header row
    // The room's topic (short) under the header line.
    std::string topic;
    for (const auto& r : st.rooms) {
        if (r.value("room_id", "") == st.currentRoomId) {
            topic = r.value("topic", "");
            break;
        }
    }
    if (!topic.empty()) {
        out += "  " + clip(topic, W - 2) + "\n";
    }
    // Pinned messages (the "pin" command inserts m.room.pinned_events):
    // a 📌 line with the preview of the last pinned event.
    if (!st.currentRoomId.empty()) {
        auto evs = st.db->getEvents(st.currentRoomId, 300);
        for (const auto& ev : evs) {
            if (ev.type != "m.room.pinned_events") continue;
            auto pinned = ev.content.find("pinned");
            if (pinned == ev.content.end() || !pinned->is_array() ||
                pinned->empty()) continue;
            const std::string& pid = (*pinned)[0].get<std::string>();
            matrix::Event pev;
            if (st.db->getEventById(pid, pev)) {
                std::string pv = eventBodyImpl(pev);
                out += "  \xf0\x9f\x93\x8c " + clip(pv, W - 4) + "\n";
            }
            break;
        }
    }
    // Send permission (like Element): @you's level vs the room default.
    {
        int myLevel = 0;
        auto me = st.accountLabel == "demo (offline)" ? "@you" : "@" + st.accountLabel;
        auto it = st.powerLevels.find(me);
        if (it != st.powerLevels.end()) myLevel = it->second;
        if (myLevel < st.eventsDefault && !st.currentRoomId.empty()) {
            out += "  \x1b[31m[read-only: level " + std::to_string(st.eventsDefault)
                 + " required to send (you: " + std::to_string(myLevel) + ")]\x1b[0m\n";
        }
    }
    // Smartphone top bar: the logged-in account, the open-invite count and
    // where we are (the space name, or "all rooms" when everything shows).
    if (st.mobile) {
        std::string where = "all rooms";
        if (!st.activeSpace.empty()) {
            for (const auto& r : st.rooms) {
                if (r.value("room_id", "") == st.activeSpace) {
                    where = r.value("name", "space");
                    break;
                }
            }
        }
        std::string top = st.accountLabel;
        if (st.invites > 0) {
            top += " \xf0\x9f\x93\xa5 " + std::to_string(st.invites) + " invites";
        }
        top += " \xc2\xb7 " + where;
        out += "\x1b[90m  " + clip(top, W - 2) + "\x1b[0m\n";
        // The space strip (Element Classic): one tap selects the space.
        if (st.mobileTab == 0) {
            std::string strip = " [all] ";
            for (const auto& r : st.rooms) {
                if (!r.value("is_space", false)) continue;
                bool active = r.value("room_id", "") == st.activeSpace;
                std::string label = clip(r.value("name", "?"), 12);
                if (active) {
                    strip += "\x1b[7m" + label + "\x1b[0m ";
                } else {
                    strip += label + " ";
                }
            }
            out += "\x1b[36m" + clip(strip, W - 2) + "\x1b[0m\n";
        }
    }
    std::string leftHeader = st.accountLabel + " — Rooms";
    if (st.invites > 0 && st.showInvites) {
        leftHeader += (st.showEmoji ? " 📥 " : " [inv] ") + std::to_string(st.invites)
                    + (st.showEmoji && st.showInvitesLegend ? " (invites)" : "");
    }
    std::string headRoom = " " + roomName;
    if (static_cast<int>(headRoom.size()) > centerW - 1) headRoom = headRoom.substr(0, centerW - 1);
    const char* PIPE = "\x1b[90m";  // dim grey for the panel pipes
    const char* X = "\x1b[0m";
    if (!st.mobile) {
        const char* rightTitle = st.rightPanel == 2 ? "Thread"
                                 : st.rightPanel == 1 ? "Threads"
                                 : st.rightPanel == 3 ? "All threads"
                                 : st.rightPanel == 4 ? "Agent"
                                 : "Members";
        out += pad(leftHeader, static_cast<size_t>(leftW)) + PIPE + "|" + X
             + pad(headRoom, static_cast<size_t>(centerW)) + PIPE + "|" + X
             + std::string(rightTitle) + "\n";
        out += PIPE + repeat('-', leftW) + "+" + repeat('-', centerW) + "+"
             + repeat('-', std::max(0, rightW - 1)) + X + "\n";
    }

    // Body rows: fill the terminal height or default to 24 rows.
    // The "rows <n>" setting (0 = auto) overrides the height; the room
    // list then scrolls inside the window (up/down/top/bottom/scroll n).
    int rows = 24;
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 6) {
        rows = static_cast<int>(ws.ws_row) - 5;
    }
#endif
    if (st.termH > 0) rows = std::max(1, st.termH - 5);
    if (st.limitRows > 0) rows = st.limitRows;

    // Room filter (find <q> / space <q>): the left panel shows only the
    // matching rooms; the center/members keep their own (unfiltered) view.
    // Space rooms are never listed as rooms; with a space selected only
    // that space's children show.
    // Hidden rooms: hide for hiddenSeconds (default 12s), then return.
    // Computed locally — drawFrame takes a const state.
    std::unordered_set<std::string> hiddenNow = st.hiddenRooms;
    if (!st.hiddenUntil.empty()) {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (const auto& [rid, until] : st.hiddenUntil) {
            if (now >= until) hiddenNow.erase(rid);
        }
    }
    std::vector<const nlohmann::json*> visible;
    for (const auto& r : st.rooms) {
        if (r.value("is_space", false)) continue;
        if (hiddenNow.count(r.value("room_id", ""))) continue;
        if (!st.activeSpace.empty() &&
            r.value("space", "") != st.activeSpace) continue;
        if (st.roomFilter.empty()) {
            visible.push_back(&r);
        } else {
            std::string id = r.value("room_id", "");
            std::string name = r.value("name", "");
            if (name.find(st.roomFilter) != std::string::npos ||
                id.find(st.roomFilter) != std::string::npos) {
                visible.push_back(&r);
            }
        }
    }
    // Pre-build the center panel rows: one per message, with date separators
    // ("── Today ──") and the message time, so the viewport scrolls over

    int scroll = st.scroll + st.scrollStep
             + st.scrollPage * rows;  // < 0 = from the bottom, clamped inside
    return drawFrameChatImpl(st, centerW, horizMembers, W, leftW, rightW,
                             scroll, rows, PIPE, X, std::move(out), visible,
                             roomName);
}

std::string drawFrame(const UiState& st) { return drawFrameImpl(st); }
void loadRoomIntoState(UiState& st, const std::string& query) {
    loadRoomIntoStateImpl(st, query);
}
int terminalWidth() { return terminalWidthImpl(); }
std::string roomDisplayName(const nlohmann::json& r) {
    UiState st;
    return roomDisplayNameImpl(st, r);
}
std::string senderShort(const std::string& sender) { return senderShortImpl(sender); }
int contentRows(const UiState& st) { return contentRowsImpl(st); }

// The public wrapper over the anonymous-namespace renderer — the llm CLI
// shares the same ANSI markdown rendering as the chat view.

std::vector<std::string> wrapText(const std::string& s, int width) {
    return wrapTextImpl(s, width);
}
int64_t parseDayMs(const std::string& s) { return parseDayMsImpl(s); }
std::string eventBody(const matrix::Event& ev) { return eventBodyImpl(ev); }
int centerRowIndexOf(const UiState& st, const std::string& eventId) {
    return centerRowIndexOfImpl(st, eventId);
}

std::string renderMarkdownAnsi(const std::string& body) {
    return renderMarkdownBody(body);
}


int cmdAttachFile(const cli::Args& args);

// ---- Local (offline) file message ----
// Used by the demo REPL and the ui REPL when no session is available:
// inserts an m.file event into the local DB (no upload, no account).
int uiInsertLocalFile(db::Database& dbi, const std::string& roomId,
                      const std::string& path, const std::string& caption,
                      const std::string& threadRoot) {
    std::string fn = path;
    auto slash = fn.find_last_of('/');
    if (slash != std::string::npos) fn = fn.substr(slash + 1);
    std::uintmax_t size = 0;
    try {
        size = std::filesystem::file_size(path);
    } catch (...) {}
    int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    matrixcli::matrix::Event ev;
    ev.event_id = "$ui_" + std::to_string(ts);
    ev.room_id = roomId;
    ev.sender = "@you:demo.local";
    ev.type = "m.room.message";
    nlohmann::json content = {
        {"msgtype", "m.file"},
        {"body", caption.empty() ? fn : caption},
        {"filename", fn},
        {"url", "mxc://demo.local/local-file"},
        {"info", {{"size", size}, {"mimetype", "application/octet-stream"}}},
    };
    if (!threadRoot.empty()) {
        content["m.relates_to"]["rel_type"] = "m.thread";
        content["m.relates_to"]["event_id"] = threadRoot;
    }
    ev.content = content;
    ev.origin_server_ts = ts;
    dbi.insertEvent(ev);
    return 0;
}

} // namespace matrixcli

// Register the ASCII UI command (via the extensible registry). Defined at
// the GLOBAL scope, matching the other register* functions.
void registerAsciiUiCommand() {
    matrixcli::CommandRegistry::instance().registerCli(
        "about", matrixcli::cmdAbout,
        "Show the about screen (logo, version, proxy, account)");
    matrixcli::CommandRegistry::instance().registerCli(
        "ui", matrixcli::cmdAsciiUi,
        "ASCII-drawn client interface (rooms | chat | members)");
    matrixcli::CommandRegistry::instance().registerCli(
        "attach", matrixcli::cmdAttachFile,
        "Send a file: attach <room> <file> [--caption text]");
    matrixcli::CommandRegistry::instance().registerCli(
        "send-file", matrixcli::cmdAttachFile,
        "Send a file (alias of attach)");
} // namespace matrixcli

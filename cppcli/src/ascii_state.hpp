#pragma once

// The shared ASCII-UI state + the rendering helpers (split out of
// ascii_ui.cpp so the settings commands live in their own TU).

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

#include "../lib/matrix/events.hpp"
#include "../lib/database/db.hpp"
#include "cli/args.hpp"

namespace matrixcli {

struct UiState {
    db::Database* db = nullptr;
    std::vector<nlohmann::json> rooms;   // listRooms()
    std::string currentRoomId;
    std::vector<matrix::Event> messages; // getEvents(currentRoom)
    std::vector<std::string> members;    // unique senders in the room
    int limit = 200;  // the chat window — deep enough for the threads
    int scroll = 0;                      // viewport offset (rows)
    int leftScroll = 0;                  // rooms-list-only offset (desktop)
    int threadsScroll = 0;               // threads-section offset (desktop right)
    int viaLimit = 3;                    // via args in permalinks; 0 = all
    int tzOffset = 0;                    // timezone offset in hours (display)
    int hiddenSeconds = 12;              // hide duration; 0 = until reload
    std::string senderFilter;            // "from @user": only their messages
    std::unordered_set<std::string> hiddenRooms;   // temporarily hidden
    std::unordered_set<std::string> mutedRooms;    // no unread/indicators
    std::unordered_set<std::string> starredRooms;  // ★ pinned to the top
    std::map<std::string, std::string> roomNicks;  // "room|user" -> display name
    std::map<std::string, std::string> roomAvatars; // room -> avatar url
    std::map<std::string, std::string> userColors;  // @user -> color name
    std::map<std::string, int64_t> hiddenUntil;    // room -> un-hide timestamp
    std::string accountLabel;            // e.g. "bob@matrix.org" or "demo (offline)"
    std::string proxyLabel;              // "on (socks5h ...)" or "off"
    std::string roomFilter;              // find/space filter for the left panel
    std::string statusNote;              // last action's summary (dump etc.)
    bool staticFrame = false;            // --static: one-shot frame
    bool mobile = false;                 // smartphone: stacked sections
    int invites = 0;                     // open invites for the logged-in user
    std::string activeSpace;             // "" = all rooms; else a space id
    bool autoPanels = true;             // size the panels to the content
    int membersMode = 0;                // 0 auto, 1 horizontal, 2 vertical list
    bool showThreadsBottom = true;      // thread list at the right panel bottom
    std::unordered_set<std::string> invited;  // rooms with an open invite
    std::map<std::string, db::InviteInfo> inviteByRoom;  // invite metadata
    std::string readMarker;                  // the last-read event id
    std::string focusEvent;              // event the viewport jumped to (goto)
    int mobileTab = 0;                   // 0=Rooms 1=Chat 2=People (bottom nav)
    int limitRows = 0;                   // settings "rows <n>": 0 = fit terminal
    std::map<std::string, std::string> presence; // member -> О/А/Ф letters
    std::map<std::string, std::string> memberNames; // member -> displayname
    // Right panel mode: 0 = members, 1 = room thread list, 2 = one thread,
    // 3 = threads across all rooms (Element-style thread panel).
    bool showIds = false;       // show event ids next to the messages
    bool showSeconds = false;   // HH:MM:SS instead of HH:MM
    bool showImages = false;    // full image cards (default: compact marker)
    bool showEmoji = true;      // emoji glyphs; off = ASCII fallbacks
    bool showNames = true;      // Element: show sender display names
    bool showReceipts = true;   // Element: show read receipts
    bool showJoins = true;      // Element: show join/leave messages
    bool showLinks = true;      // Element: enable URL previews (the pills)
    bool clock12h = false;      // Element: 12/24-hour clock
    bool timeRight = false;     // chat time at the right edge of the row
    bool msgNewline = false;    // message on its own line under "HH:MM [nick] >"
    std::unordered_set<std::string> nameColliders;  // same localpart → full mxid
    int leftPanelW = -1;        // -1 = default width, 0 = hidden
    int rightPanelW = -1;       // -1 = default width, 0 = hidden
    std::map<std::string, int> powerLevels;  // member -> power level
    nlohmann::json powerLevelsEvent;         // the full m.room.power_levels
    int eventsDefault = 0;               // the room's send permission level
    std::unordered_set<std::string> redactedIds;  // events that were redacted
    std::unordered_set<std::string> pinned;       // the room's pinned ids
    std::map<std::string, std::string> receipts;  // eventId -> "a b" readers
    int rightPanel = 0;
    std::string threadRoomId;   // for the room thread list
    std::string threadRootId;   // for the single-thread view
    std::vector<matrix::Event> threadReplies;  // replies of threadRootId
};

// The rendering + the state helpers (defined in ascii_ui.cpp).
std::string drawFrame(const UiState& st);
void loadRoomIntoState(UiState& st, const std::string& query);
int terminalWidth();
std::string roomDisplayName(const nlohmann::json& r);
std::string senderShort(const std::string& sender);
int contentRows(const UiState& st);

// The settings/display commands (defined in ascii_settings.cpp).
// Returns true when the command was handled.
bool asciiSettingsCommand(UiState& st, db::Database& dbi, const cli::Args& a);

} // namespace matrixcli

// The global relative-time formatter (the string_utils.hpp helpers).
std::string relativeTime(int64_t ts_ms);

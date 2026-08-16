#pragma once

#include "screen.hpp"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <mutex>

namespace matrixcli { namespace tui {

struct RoomInfo {
    std::string id;
    std::string name;
    bool has_unread = false;
    bool is_encrypted = false;
};

struct MessageInfo {
    std::string sender;
    std::string body;
    std::string event_id;
    std::string thread_id; // thread root event_id
    bool is_encrypted = false;
    bool is_notice = false;
    bool is_emote = false;
    std::string reaction;
    int receipts = 0;        // the users who read up to here (✔ N)
    bool is_highlight = false;
    bool is_edited = false;
    bool is_redacted = false;
    std::string redacted_by;
    std::string url;
    std::string mimetype;
    bool is_thread_root = false;
    int thread_reply_count = 0;
    bool is_poll = false;
    std::string poll_question;
    std::vector<std::pair<std::string, int>> poll_options; // text, votes
    bool poll_ended = false;
    std::string url_title;
    std::string url_desc;
};

struct MemberInfo {
    std::string user_id;
    std::string display_name;
    int power_level = 0;
    std::string membership; // join, invite, leave
};

class ChatView {
public:
    ChatView();
    ~ChatView();

    using SendCallback = std::function<void(const std::string& body)>;
    using RoomSwitchCallback = std::function<void(const std::string& room_id)>;

    // Data
    void setRooms(const std::vector<RoomInfo>& rooms);
    void addRoom(const RoomInfo& room);
    void setMessages(const std::string& room_id, const std::vector<MessageInfo>& msgs,
                     const std::string& thread_root = "");
    void addMessage(const std::string& room_id, const MessageInfo& msg);
    void setTypingUsers(const std::string& room_id, const std::vector<std::string>& users);

    // The read-receipt count for one message (the m.receipt events).
    void setReceipts(const std::string& room_id, const std::string& event_id,
                     int count);

    // Interaction
    void setSendCallback(SendCallback cb) { _sendCb = std::move(cb); }
    void setRoomSwitchCallback(RoomSwitchCallback cb) { _roomSwitchCb = std::move(cb); }
    void setStatus(const std::string& status) { _status = status; }
    void setMembers(const std::string& room_id, const std::vector<MemberInfo>& members);
    void setNotificationCallback(std::function<void(const std::string&)> cb) { _notifyCb = std::move(cb); }
    void setConnectionStatus(const std::string& status) { _connStatus = status; _needsRedraw = true; }

    // Commands
    using CommandHandler = std::function<void(const std::string& cmd, const std::string& args)>;
    void setCommandHandler(CommandHandler cb) { _cmdHandler = std::move(cb); }

    // The Ctrl+F message search: the query is handed to the callback on
    // Enter (the caller filters the chat and reports the match count).
    using SearchCallback = std::function<void(const std::string& query)>;
    void setSearchCallback(SearchCallback cb) { _searchCb = std::move(cb); }

    // The typing notification hook: fired when the composer receives a
    // printable key (the caller throttles + respects the send_typing
    // setting).
    void setTypeNotify(std::function<void()> cb) { _typeCb = std::move(cb); }

    // Esc pressed while the input is focused (used to interrupt a running
    // agent — the TUI sets the g_agentInterrupt flag).
    void setEscHandler(std::function<void()> cb) { _escCb = std::move(cb); }

    // Polled every loop iteration; returning true exits the TUI (the
    // Ctrl+C handler — the TUI watches the SIGINT flag).
    void setQuitCheck(std::function<bool()> cb) { _quitCb = std::move(cb); }

    // Pagination
    using PaginateCallback = std::function<void(const std::string& room_id)>;
    void setPaginateCallback(PaginateCallback cb) { _paginateCb = std::move(cb); }

    // Unread
    int getUnreadCount(const std::string& room_id) const;

    // Main loop
    void run(Screen& screen);
    void stop() { _running = false; }
    void requestRedraw() { _needsRedraw = true; }

    // For external sync callback
    std::string activeRoomId() const;

private:
    void draw(Screen& screen);
    void drawRoomList(Screen& screen, int x, int y, int w, int h);
    void drawMemberList(Screen& screen, int x, int y, int w, int h);
    void drawMessages(Screen& screen, int x, int y, int w, int h);
    void drawInput(Screen& screen, int x, int y, int w);
    void drawStatus(Screen& screen, int y, int w);
    void drawSearch(Screen& screen, int x, int y, int w);
    void handleKey(Screen& screen, int key);

    void loadMessagesForActiveRoom();

    // State
    std::vector<RoomInfo> _rooms;
    std::map<std::string, std::vector<MessageInfo>> _messages;
    std::map<std::string, std::vector<MemberInfo>> _members;
    std::string _activeRoom;
    std::string _activeThread; // empty = main timeline, event_id = thread view
    int _roomScroll = 0;
    int _msgScroll = 0;
    std::string _input;
    int _cursorPos = 0;
    enum { FOCUS_INPUT, FOCUS_ROOMS, FOCUS_SEARCH } _focus = FOCUS_INPUT;
    enum { PANE_ROOMS, PANE_MEMBERS } _leftPane = PANE_ROOMS;
    bool _showHelp = false;
    bool _showSearch = false;
    std::string _searchQuery;
    std::string _status;
    std::string _connStatus;
    std::map<std::string, std::vector<std::string>> _typingUsers;
    std::map<std::string, int> _unreadCounts;
    bool _running = true;
    bool _needsRedraw = true;

    SendCallback _sendCb;
    RoomSwitchCallback _roomSwitchCb;
    std::function<void(const std::string&)> _notifyCb;
    CommandHandler _cmdHandler;
    std::function<void()> _escCb;
    std::function<bool()> _quitCb;
    PaginateCallback _paginateCb;
    SearchCallback _searchCb;
    std::function<void()> _typeCb;
    mutable std::mutex _mutex;
};

}} // namespace matrixcli::tui

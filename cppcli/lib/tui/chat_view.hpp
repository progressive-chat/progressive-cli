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
    bool is_encrypted = false;
    bool is_notice = false;
    bool is_emote = false;
    std::string reaction;
    bool is_highlight = false;
    bool is_edited = false;
    bool is_redacted = false;
    std::string redacted_by = "";
    std::string url; // mxc:// or https:// URL for images/files
    std::string mimetype;
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
    void setMessages(const std::string& room_id, const std::vector<MessageInfo>& msgs);
    void addMessage(const std::string& room_id, const MessageInfo& msg);

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
    void drawSearch(Screen& screen, int w, int h);
    void handleKey(Screen& screen, int key);

    void loadMessagesForActiveRoom();

    // State
    std::vector<RoomInfo> _rooms;
    std::map<std::string, std::vector<MessageInfo>> _messages;
    std::map<std::string, std::vector<MemberInfo>> _members;
    std::string _activeRoom;
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
    bool _running = true;
    bool _needsRedraw = true;

    SendCallback _sendCb;
    RoomSwitchCallback _roomSwitchCb;
    std::function<void(const std::string&)> _notifyCb;
    CommandHandler _cmdHandler;
    mutable std::mutex _mutex;
};

}} // namespace matrixcli::tui

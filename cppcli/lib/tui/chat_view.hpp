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

    // Main loop
    void run(Screen& screen);
    void stop() { _running = false; }
    void requestRedraw() { _needsRedraw = true; }

    // For external sync callback
    std::string activeRoomId() const;

private:
    void draw(Screen& screen);
    void drawRoomList(Screen& screen, int x, int y, int w, int h);
    void drawMessages(Screen& screen, int x, int y, int w, int h);
    void drawInput(Screen& screen, int x, int y, int w);
    void drawStatus(Screen& screen, int y, int w);
    void handleKey(Screen& screen, int key);

    void loadMessagesForActiveRoom();

    // State
    std::vector<RoomInfo> _rooms;
    std::map<std::string, std::vector<MessageInfo>> _messages;
    std::string _activeRoom;
    int _roomScroll = 0;
    int _msgScroll = 0;
    std::string _input;
    int _cursorPos = 0;
    enum { FOCUS_INPUT, FOCUS_ROOMS } _focus = FOCUS_INPUT;
    std::string _status;
    bool _running = true;
    bool _needsRedraw = true;

    SendCallback _sendCb;
    RoomSwitchCallback _roomSwitchCb;
    mutable std::mutex _mutex;
};

}} // namespace matrixcli::tui

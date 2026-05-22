#include "chat_view.hpp"
#include "help_screen.hpp"

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif

#include <algorithm>
#include <thread>
#include <chrono>

namespace matrixcli { namespace tui {

ChatView::ChatView() {}
ChatView::~ChatView() { stop(); }

void ChatView::setRooms(const std::vector<RoomInfo>& rooms) {
    std::lock_guard<std::mutex> lock(_mutex);
    _rooms = rooms;
    if (!_rooms.empty() && _activeRoom.empty()) {
        _activeRoom = _rooms[0].id;
    }
    _needsRedraw = true;
}

void ChatView::addRoom(const RoomInfo& room) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& r : _rooms) {
        if (r.id == room.id) { r = room; _needsRedraw = true; return; }
    }
    _rooms.push_back(room);
    if (_activeRoom.empty()) _activeRoom = room.id;
    _needsRedraw = true;
}

void ChatView::setMessages(const std::string& room_id, const std::vector<MessageInfo>& msgs) {
    std::lock_guard<std::mutex> lock(_mutex);
    _messages[room_id] = msgs;
    _msgScroll = 0;
    _needsRedraw = true;
}

void ChatView::addMessage(const std::string& room_id, const MessageInfo& msg) {
    std::lock_guard<std::mutex> lock(_mutex);
    _messages[room_id].push_back(msg);
    if (_messages[room_id].size() > 1000) {
        _messages[room_id].erase(_messages[room_id].begin(),
                                 _messages[room_id].begin() + 200);
        if (_msgScroll >= 200) _msgScroll -= 200; else _msgScroll = 0;
    }
    _needsRedraw = true;
}

std::string ChatView::activeRoomId() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _activeRoom;
}

void ChatView::setMembers(const std::string& room_id, const std::vector<MemberInfo>& members) {
    std::lock_guard<std::mutex> lock(_mutex);
    _members[room_id] = members;
    _needsRedraw = true;
}

void ChatView::run(Screen& screen) {
    _running = true;
    _needsRedraw = true;

    while (_running) {
        if (_showHelp) {
            HelpScreen::show(screen);
            _showHelp = false;
            _needsRedraw = true;
            continue;
        }

        draw(screen);

        int key = screen.getKey();
        if (key == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        handleKey(screen, key);
    }
}

void ChatView::draw(Screen& screen) {
    if (!_needsRedraw) return;
    _needsRedraw = false;

#ifdef HAS_NCURSES
    werase(stdscr);
#endif

    int w = screen.width();
    int h = screen.height();

    // Status bar (line 0)
    drawStatus(screen, 0, w);

    // Room list or Member list (left panel)
    int room_w = std::min(25, w / 4);
    int room_h = h - 3;
    if (_leftPane == PANE_MEMBERS) {
        drawMemberList(screen, 1, 1, room_w, room_h);
    } else {
        drawRoomList(screen, 1, 1, room_w, room_h);
    }

    // Messages (right panel)
    int msg_x = room_w + 2;
    int msg_w = w - msg_x - 1;
    drawMessages(screen, msg_x, 1, msg_w, room_h);

    // Separator line
#ifdef HAS_NCURSES
    for (int y = 1; y <= room_h; y++) {
        mvaddch(y, room_w, ACS_VLINE);
    }
    mvhline(0, 0, ACS_HLINE, w);
    mvhline(room_h + 1, 0, ACS_HLINE, w);
#endif

    // Input line (bottom)
    drawInput(screen, 1, room_h + 1, w - 1);

    screen.refresh();
}

void ChatView::drawStatus(Screen& screen, int y, int w) {
    std::string status = _status;
    if (status.empty()) status = "matrixcli";
    if (status.size() > (size_t)w) status = status.substr(0, w);

#ifdef HAS_NCURSES
    attron(A_REVERSE);
    mvaddstr(y, 0, status.c_str());
    for (int x = status.size(); x < w; x++) mvaddch(y, x, ' ');
    attroff(A_REVERSE);
#else
    screen.drawText(0, y, status);
#endif
}

void ChatView::drawRoomList(Screen& screen, int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(_mutex);

#ifdef HAS_NCURSES
    // Title
    attron(A_BOLD);
    mvaddstr(y, x, "Rooms");
    attroff(A_BOLD);

    int visible = h - 2;
    int total = _rooms.size();
    if (_roomScroll > total - visible) _roomScroll = std::max(0, total - visible);
    if (_roomScroll < 0) _roomScroll = 0;

    for (int i = 0; i < visible; i++) {
        int idx = _roomScroll + i;
        int ry = y + 1 + i;

        if (idx >= total) {
            for (int cx = x; cx < x + w; cx++) mvaddch(ry, cx, ' ');
            continue;
        }

        auto& room = _rooms[idx];
        std::string label = room.name.empty() ? room.id : room.name;
        if (label.size() > (size_t)(w - 3)) label = label.substr(0, w - 3);

        bool is_active = (room.id == _activeRoom);
        bool focused = (_focus == FOCUS_ROOMS);

        if (is_active) attron(A_BOLD);
        if (room.has_unread) attron(A_UNDERLINE);
        if (room.is_encrypted) label += " *";

        if (is_active && focused) {
            attron(A_REVERSE);
            mvprintw(ry, x, " %-*s ", w - 2, label.c_str());
            attroff(A_REVERSE);
        } else {
            mvprintw(ry, x, " %-*s ", w - 2, label.c_str());
        }

        if (room.has_unread) attroff(A_UNDERLINE);
        if (is_active) attroff(A_BOLD);
    }
#else
    screen.drawText(x, y, "Rooms");
    int i = 1;
    for (auto& room : _rooms) {
        if (i >= h) break;
        std::string label = room.name.empty() ? room.id : room.name;
        if (room.id == _activeRoom) label = "> " + label;
        else label = "  " + label;
        screen.drawText(x, y + i, label);
        i++;
    }
#endif
}

void ChatView::drawMemberList(Screen& screen, int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(_mutex);

#ifdef HAS_NCURSES
    attron(A_BOLD);
    mvaddstr(y, x, "Members");
    attroff(A_BOLD);

    auto it = _members.find(_activeRoom);
    if (it == _members.end()) {
        mvaddstr(y + 1, x, "(sync first)");
        return;
    }

    auto& members = it->second;
    int visible = h - 2;
    for (int i = 0; i < visible && i < (int)members.size(); i++) {
        auto& m = members[i];
        int ry = y + 1 + i;
        std::string label = m.display_name.empty() ? m.user_id : m.display_name;
        if (m.power_level >= 100) label = "@" + label;
        if (label.size() > (size_t)(w - 3)) label = label.substr(0, w - 3);
        if (m.membership == "join") attron(COLOR_PAIR(2));
        else attron(A_DIM);
        mvprintw(ry, x, " %-*s ", w - 2, label.c_str());
        if (m.membership == "join") attroff(COLOR_PAIR(2));
        else attroff(A_DIM);
    }
#else
    screen.drawText(x, y, "Members");
#endif
}

void ChatView::drawMessages(Screen& screen, int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _messages.find(_activeRoom);
    if (it == _messages.end()) {
        screen.drawText(x, y, "(no messages)");
        return;
    }

    auto& msgs = it->second;
    int visible = h - 1;
    int total = msgs.size();
    if (_msgScroll >= total) _msgScroll = std::max(0, total - visible);
    if (_msgScroll < 0) _msgScroll = 0;

    for (int i = 0; i < visible; i++) {
        int idx = total - 1 - (_msgScroll + i);
        int ry = y + i;

        if (idx < 0 || idx >= total) {
#ifdef HAS_NCURSES
            for (int cx = x; cx < x + w; cx++) mvaddch(ry, cx, ' ');
#endif
            continue;
        }

        auto& msg = msgs[idx];
        std::string prefix = msg.sender + ": ";
        std::string suffix;
        if (msg.is_emote) prefix = "* " + msg.sender + " ";
        else if (msg.is_notice) prefix = "[!] " + msg.sender + ": ";
        else if (msg.is_encrypted) prefix = msg.sender + ": [encrypted] ";
        if (!msg.reaction.empty()) suffix = "  " + msg.reaction;
        if (msg.is_highlight) prefix = "★ " + prefix;

        int avail = w - (int)prefix.size();
        if (avail <= 0) continue;
        std::string line = msg.body.substr(0, avail);

#ifdef HAS_NCURSES
        if (msg.is_emote) attron(A_ITALIC);
        if (msg.is_notice) attron(A_DIM);

        attron(COLOR_PAIR(1)); // sender color
        mvaddstr(ry, x, prefix.c_str());
        attroff(COLOR_PAIR(1));

        mvaddstr(ry, x + prefix.size(), line.c_str());

        if (msg.is_notice) attroff(A_DIM);
        if (msg.is_emote) attroff(A_ITALIC);
#else
        screen.drawText(x, ry, prefix + line);
#endif
    }
}

void ChatView::drawInput(Screen& screen, int x, int y, int w) {
#ifdef HAS_NCURSES
    mvaddch(y, x - 1, '>');
    mvaddstr(y, x, _input.c_str());
    // Pad rest
    for (int cx = x + _input.size(); cx < x + w; cx++) mvaddch(y, cx, ' ');

    // Cursor
    if (_focus == FOCUS_INPUT) {
        int cx = x + _cursorPos;
        if (cx >= x + w) cx = x + w - 1;
        move(y, cx);
        curs_set(1);
    } else {
        curs_set(0);
    }
#else
    screen.drawText(x, y, "> " + _input);
#endif
}

void ChatView::handleKey(Screen& screen, int key) {
#ifdef HAS_NCURSES
    if (_focus == FOCUS_INPUT) {
        switch (key) {
        case '\n': case '\r': case KEY_ENTER:
            if (!_input.empty() && _sendCb) {
                std::string text = _input;
                _input.clear();
                _cursorPos = 0;
                _sendCb(text);
                _needsRedraw = true;
            }
            break;
        case KEY_BACKSPACE: case 127: case '\b':
            if (_cursorPos > 0) {
                _input.erase(_input.begin() + _cursorPos - 1);
                _cursorPos--;
                _needsRedraw = true;
            }
            break;
        case KEY_DC:
            if (_cursorPos < (int)_input.size()) {
                _input.erase(_input.begin() + _cursorPos);
                _needsRedraw = true;
            }
            break;
        case KEY_LEFT:
            if (_cursorPos > 0) { _cursorPos--; _needsRedraw = true; }
            break;
        case KEY_RIGHT:
            if (_cursorPos < (int)_input.size()) { _cursorPos++; _needsRedraw = true; }
            break;
        case KEY_HOME:
            _cursorPos = 0; _needsRedraw = true;
            break;
        case KEY_END:
            _cursorPos = _input.size(); _needsRedraw = true;
            break;
        case '\t': // Tab - switch focus
            _focus = FOCUS_ROOMS;
            _needsRedraw = true;
            break;
        case KEY_UP: {
            // Scroll messages up
            std::lock_guard<std::mutex> lock(_mutex);
            _msgScroll++;
            _needsRedraw = true;
            break;
        }
        case KEY_DOWN: {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_msgScroll > 0) _msgScroll--;
            _needsRedraw = true;
            break;
        }
        case KEY_NPAGE: {
            std::lock_guard<std::mutex> lock(_mutex);
            _msgScroll += screen.height() / 2;
            _needsRedraw = true;
            break;
        }
        case KEY_PPAGE: {
            std::lock_guard<std::mutex> lock(_mutex);
            _msgScroll -= screen.height() / 2;
            if (_msgScroll < 0) _msgScroll = 0;
            _needsRedraw = true;
            break;
        }
        case 'q': case 'Q':
            if (_input.empty()) {
                _running = false;
            } else {
                _input += (char)key;
                _cursorPos++;
                _needsRedraw = true;
            }
            break;
        case 'm': case 'M':
            _leftPane = (_leftPane == PANE_ROOMS) ? PANE_MEMBERS : PANE_ROOMS;
            _needsRedraw = true;
            break;
        case '?':
            _showHelp = true;
            _needsRedraw = true;
            break;
        default:
            if (key >= 32 && key <= 126) {
                _input.insert(_input.begin() + _cursorPos, (char)key);
                _cursorPos++;
                _needsRedraw = true;
            }
            break;
        }
    } else {
        // Focus is on room list
        switch (key) {
        case KEY_UP:
            if (_roomScroll > 0) { _roomScroll--; _needsRedraw = true; }
            break;
        case KEY_DOWN:
            if (_roomScroll < (int)_rooms.size() - 1) { _roomScroll++; _needsRedraw = true; }
            break;
        case '\n': case '\r': case KEY_ENTER:
        case ' ': {
            std::lock_guard<std::mutex> lock(_mutex);
            int idx = _roomScroll;
            if (idx >= 0 && idx < (int)_rooms.size()) {
                _activeRoom = _rooms[idx].id;
                _msgScroll = 0;
                _needsRedraw = true;
                if (_roomSwitchCb) _roomSwitchCb(_activeRoom);
            }
            break;
        }
        case '\t': // Switch back to input
            _focus = FOCUS_INPUT;
            _needsRedraw = true;
            break;
        case 'q': case 'Q':
            _running = false;
            break;
        case 'm': case 'M':
            _leftPane = (_leftPane == PANE_ROOMS) ? PANE_MEMBERS : PANE_ROOMS;
            _needsRedraw = true;
            break;
        case '?':
            _showHelp = true;
            _needsRedraw = true;
            break;
        case 'j':
            if (_roomScroll < (int)_rooms.size() - 1) { _roomScroll++; _needsRedraw = true; }
            break;
        case 'k':
            if (_roomScroll > 0) { _roomScroll--; _needsRedraw = true; }
            break;
        }
    }
#else
    (void)screen;
    if (key == 'q') _running = false;
#endif
}

}} // namespace matrixcli::tui

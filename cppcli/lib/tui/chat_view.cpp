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

void ChatView::setTypingUsers(const std::string& room_id, const std::vector<std::string>& users) {
    std::lock_guard<std::mutex> lock(_mutex);
    _typingUsers[room_id] = users;
    _needsRedraw = true;
}

void ChatView::setMessages(const std::string& room_id, const std::vector<MessageInfo>& msgs,
                            const std::string& thread_root) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string key = thread_root.empty() ? room_id : room_id + ":" + thread_root;
    _messages[key] = msgs;
    _msgScroll = 0;
    _needsRedraw = true;
}

void ChatView::addMessage(const std::string& room_id, const MessageInfo& msg) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string key = msg.thread_id.empty() ? room_id : room_id + ":" + msg.thread_id;
    _messages[key].push_back(msg);
    if (_messages[key].size() > 1000) {
        _messages[key].erase(_messages[key].begin(), _messages[key].begin() + 200);
        if (_msgScroll >= 200) _msgScroll -= 200; else _msgScroll = 0;
    }
    // Increment unread if room is not active
    if (room_id != _activeRoom && !msg.is_notice) {
        _unreadCounts[room_id]++;
    }
    _needsRedraw = true;
}

int ChatView::getUnreadCount(const std::string& room_id) const {
    auto it = _unreadCounts.find(room_id);
    return it != _unreadCounts.end() ? it->second : 0;
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
    if (!_activeThread.empty()) status += " [thread]";
    if (!_connStatus.empty()) status += " | " + _connStatus;

    // Typing indicators
    auto ti = _typingUsers.find(_activeRoom);
    if (ti != _typingUsers.end() && !ti->second.empty()) {
        status += " | ";
        for (size_t i = 0; i < ti->second.size() && i < 3; i++) {
            if (i > 0) status += ", ";
            status += ti->second[i];
        }
        if (ti->second.size() > 3) status += " +" + std::to_string(ti->second.size() - 3);
        status += " typing...";
    }

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
        int unread = getUnreadCount(room.id);
        if (unread > 0) label += " (" + std::to_string(unread) + ")";
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

// Keywords for basic syntax highlighting
static const char* cpp_keywords[] = {
    "if","else","for","while","do","switch","case","break","continue","return",
    "class","struct","namespace","template","typename","using","public","private","protected",
    "const","static","virtual","override","final","auto","decltype","sizeof","new","delete",
    "int","float","double","char","bool","void","string","vector","map","unique_ptr","shared_ptr",
    "include","define","ifdef","ifndef","endif","pragma", "try","catch","throw","nullptr","true","false",
    nullptr
};

static bool isKeyword(const std::string& word) {
    for (auto* kw : cpp_keywords) {
        if (kw && word == kw) return true;
    }
    return false;
}

static void renderRichLine(int y, int x, const std::string& text, int maxw) {
#ifdef HAS_NCURSES
    int pos = 0;
    auto peek = [&](int i) -> char { return (pos + i < (int)text.size()) ? text[pos + i] : '\0'; };

    while (pos < (int)text.size() && pos < maxw) {
        // Blockquote: > text
        if (peek(0) == '>' && (pos == 0 || text[pos-1] == '\n')) {
            attron(A_DIM);
            while (pos < (int)text.size() && text[pos] != '\n') mvaddch(y, x++, text[pos++]);
            attroff(A_DIM);
            continue;
        }
        // Bold: **text**
        if (peek(0) == '*' && peek(1) == '*' && pos + 2 < (int)text.size()) {
            pos += 2;
            attron(A_BOLD);
            while (pos < (int)text.size() && !(peek(0) == '*' && peek(1) == '*')) {
                mvaddch(y, x++, text[pos++]);
            }
            attroff(A_BOLD);
            if (peek(0) == '*' && peek(1) == '*') pos += 2;
            continue;
        }
        // Italic: *text*
        if (peek(0) == '*' && peek(1) != '*' && pos > 0 && (text[pos-1] == ' ' || text[pos-1] == '\n')) {
            pos++;
            attron(A_ITALIC);
            while (pos < (int)text.size() && text[pos] != '*') {
                mvaddch(y, x++, text[pos++]);
            }
            attroff(A_ITALIC);
            if (pos < (int)text.size()) pos++;
            continue;
        }
        // Code: `text`
        if (peek(0) == '`') {
            pos++;
            attron(COLOR_PAIR(3));
            while (pos < (int)text.size() && text[pos] != '`') {
                mvaddch(y, x++, text[pos++]);
            }
            attroff(COLOR_PAIR(3));
            if (pos < (int)text.size()) pos++;
            continue;
        }
        // URL
        if ((peek(0) == 'h' && peek(1) == 't' && peek(2) == 't' && peek(3) == 'p') ||
            (peek(0) == 'm' && peek(1) == 'x' && peek(2) == 'c')) {
            attron(COLOR_PAIR(2) | A_UNDERLINE);
            while (pos < (int)text.size() && text[pos] != ' ' && text[pos] != '\n') {
                mvaddch(y, x++, text[pos++]);
            }
            attroff(COLOR_PAIR(2) | A_UNDERLINE);
            continue;
        }
        mvaddch(y, x++, text[pos++]);
    }
#else
    (void)y; (void)x; (void)text; (void)maxw;
#endif
}

void ChatView::drawMessages(Screen& screen, int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lock(_mutex);

    std::string msgKey = _activeThread.empty() ? _activeRoom : _activeRoom + ":" + _activeThread;
    auto it = _messages.find(msgKey);
    if (it == _messages.end()) {
        screen.drawText(x, y, _activeThread.empty() ? "(no messages)" : "(no thread replies)");
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
        if (msg.is_edited) suffix += " (edited)";
        if (msg.is_thread_root && msg.thread_reply_count > 0) suffix += " [" + std::to_string(msg.thread_reply_count) + " replies]";
        if (!msg.thread_id.empty() && _activeThread.empty()) prefix = "↳ " + prefix;
        if (msg.is_poll) {
            prefix = "[POLL] " + prefix;
            suffix += msg.poll_ended ? " [closed]" : " [open]";
        }

        int avail = w - (int)prefix.size() - (int)suffix.size();
        if (avail <= 0) continue;
        std::string line = msg.body.substr(0, avail);

        if (msg.is_redacted) {
            attron(A_DIM);
            mvaddstr(ry, x, (prefix + line + suffix).c_str());
            attroff(A_DIM);
        } else {
#ifdef HAS_NCURSES
            if (msg.is_emote) attron(A_ITALIC);
            if (msg.is_notice) attron(A_DIM);
            if (msg.is_highlight) attron(A_BOLD);

            attron(COLOR_PAIR(1));
            mvaddstr(ry, x, prefix.c_str());
            attroff(COLOR_PAIR(1));

            // Basic rich text: **bold**, *italic*, `code`, URLs
            renderRichLine(ry, x + prefix.size(), line, avail);

            if (!suffix.empty()) {
                attron(A_DIM);
                mvaddstr(ry, x + prefix.size() + line.size(), suffix.c_str());
                attroff(A_DIM);
            }

            if (msg.is_highlight) attroff(A_BOLD);
            if (msg.is_notice) attroff(A_DIM);
            if (msg.is_emote) attroff(A_ITALIC);
#else
            mvaddstr(ry, x, (prefix + line + suffix).c_str());
#endif
        }
        // Poll options
        if (msg.is_poll && !msg.poll_options.empty()) {
            for (size_t pi = 0; pi < msg.poll_options.size() && (ry + 1 + pi) < (y + h); pi++) {
                auto& opt = msg.poll_options[pi];
                int maxVotes = 0;
                for (auto& o : msg.poll_options) maxVotes = std::max(maxVotes, o.second);
                int barLen = maxVotes > 0 ? (opt.second * 10 / maxVotes) : 0;
                std::string bar = " [" + std::string(barLen, '#') + std::string(10 - barLen, ' ') + "] " +
                                  std::to_string(opt.second) + " " + opt.first;
                mvaddstr(ry + 1 + pi, x + 2, bar.c_str());
            }
        }
        (void)screen;
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
            if (!_input.empty()) {
                if (_input[0] == '/' && _cmdHandler) {
                    // Slash command
                    auto space = _input.find(' ');
                    std::string cmd = _input.substr(1, space - 1);
                    std::string args = (space != std::string::npos) ? _input.substr(space + 1) : "";
                    _cmdHandler(cmd, args);
                    _input.clear();
                    _cursorPos = 0;
                    _needsRedraw = true;
                } else if (_sendCb) {
                    std::string text = _input;
                    _input.clear();
                    _cursorPos = 0;
                    _needsRedraw = true;
                    _sendCb(text);
                }
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
            std::lock_guard<std::mutex> lock(_mutex);
            _msgScroll++;
            // Trigger pagination if scrolled to top
            std::string key = _activeThread.empty() ? _activeRoom : _activeRoom + ":" + _activeThread;
            auto it = _messages.find(key);
            if (it != _messages.end() && _msgScroll >= (int)it->second.size() && _paginateCb) {
                _paginateCb(_activeRoom);
            }
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
        case 't': case 'T':
            if (_activeThread.empty()) {
                std::string key = _activeRoom;
                auto it = _messages.find(key);
                if (it != _messages.end()) {
                    for (auto& m : it->second) {
                        if (m.is_thread_root) { _activeThread = m.event_id; _msgScroll = 0; _needsRedraw = true; break; }
                    }
                }
            } else { _activeThread.clear(); _msgScroll = 0; _needsRedraw = true; }
            break;
        case KEY_F(5): case '\x06': // Ctrl+F
            _showSearch = !_showSearch;
            if (_showSearch) {
                _focus = FOCUS_SEARCH;
                _searchQuery.clear();
            } else {
                _focus = FOCUS_INPUT;
            }
            _needsRedraw = true;
            break;
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
                _unreadCounts[_activeRoom] = 0; // clear unread
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

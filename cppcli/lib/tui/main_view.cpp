#include "main_view.hpp"
#include "screen.hpp"

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif
#include <algorithm>

namespace matrixcli { namespace tui {

MainView::MainView() = default;
MainView::~MainView() = default;

void MainView::addMessage(const std::string& sender, const std::string& body) {
    _messages.emplace_back(sender, body);
    if (_messages.size() > 1000) {
        _messages.erase(_messages.begin(), _messages.begin() + 200);
    }
}

void MainView::setStatus(const std::string& status) {
    _status = status;
}

void MainView::run(Screen& screen) {
    _running = true;
#ifdef HAS_NCURSES
    nodelay(stdscr, TRUE);
#endif

    while (_running.load()) {
        render(screen, _scroll_offset);
        int ch = screen.getKey();
        if (ch == 'q' || ch == 'Q') {
            _running = false;
        } else if (ch == KEY_UP || ch == 'k') {
            _scroll_offset = std::max(0, _scroll_offset - 1);
        } else if (ch == KEY_DOWN || ch == 'j') {
            int max_scroll = std::max(0,
                static_cast<int>(_messages.size()) - screen.height() + 3);
            _scroll_offset = std::min(max_scroll, _scroll_offset + 1);
        } else if (ch == KEY_HOME) {
            _scroll_offset = 0;
        } else if (ch == KEY_END) {
            _scroll_offset = std::max(0,
                static_cast<int>(_messages.size()) - screen.height() + 3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void MainView::stop() {
    _running = false;
}

void MainView::render(Screen& screen, int scroll_offset) {
#ifdef HAS_NCURSES
    werase(stdscr);
    int h = screen.height();
    int content_h = h - 1;
    int msg_start = scroll_offset;
    int msg_end = std::min(msg_start + content_h, static_cast<int>(_messages.size()));

    for (int i = msg_start, line = 0; i < msg_end && line < content_h; ++i, ++line) {
        std::string text = _messages[i].first + ": " + _messages[i].second;
        if (static_cast<int>(text.size()) > screen.width()) {
            text.resize(screen.width() - 1);
        }
        screen.drawText(0, line, text);
    }

    screen.drawStatusBar(_status.empty() ? "matrixcli | q:quit j/k:scroll" : _status);
#else
    (void)scroll_offset;
#endif
}

}} // namespace matrixcli::tui

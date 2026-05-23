#include "screen.hpp"

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif
#include <stdexcept>

namespace matrixcli { namespace tui {

Screen::Screen() = default;

Screen::~Screen() {
    shutdown();
}

void Screen::init() {
#ifdef HAS_NCURSES
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);     // Sender name
        init_pair(2, COLOR_GREEN, -1);    // Room name / member
        init_pair(3, COLOR_YELLOW, -1);   // URLs / links
    }
    _win = stdscr;
    _initialized = true;
#else
    _initialized = true;
#endif
}

void Screen::shutdown() {
#ifdef HAS_NCURSES
    if (_initialized) {
        endwin();
    }
#endif
    _initialized = false;
}

void Screen::refresh() {
#ifdef HAS_NCURSES
    if (_initialized) {
        wrefresh(_win);
    }
#endif
}

int Screen::width() const {
#ifdef HAS_NCURSES
    return getmaxx(_win);
#else
    return 80;
#endif
}

int Screen::height() const {
#ifdef HAS_NCURSES
    return getmaxy(_win);
#else
    return 24;
#endif
}

void Screen::drawBorder() {
#ifdef HAS_NCURSES
    box(_win, 0, 0);
#endif
}

void Screen::drawText(int x, int y, const std::string& text) {
#ifdef HAS_NCURSES
    mvwaddstr(_win, y, x, text.c_str());
#endif
}

void Screen::drawTextCentered(int y, const std::string& text) {
#ifdef HAS_NCURSES
    int x = (width() - static_cast<int>(text.size())) / 2;
    if (x < 0) x = 0;
    mvwaddstr(_win, y, x, text.c_str());
#endif
}

void Screen::drawStatusBar(const std::string& text) {
#ifdef HAS_NCURSES
    int h = height();
    wattron(_win, A_REVERSE);
    std::string bar = text;
    bar.resize(width() - 1, ' ');
    mvwaddstr(_win, h - 1, 0, bar.c_str());
    wattroff(_win, A_REVERSE);
#endif
}

int Screen::getKey() {
#ifdef HAS_NCURSES
    return wgetch(_win);
#else
    return -1;
#endif
}

}} // namespace matrixcli::tui

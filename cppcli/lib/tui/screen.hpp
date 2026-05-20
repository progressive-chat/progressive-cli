#pragma once

#include <string>
#include <functional>
#include <memory>
#include <vector>

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif

namespace matrixcli { namespace tui {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

class Screen {
public:
    Screen();
    ~Screen();

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    void init();
    void shutdown();
    void refresh();

    int width() const;
    int height() const;

    void drawBorder();
    void drawText(int x, int y, const std::string& text);
    void drawTextCentered(int y, const std::string& text);
    void drawStatusBar(const std::string& text);

    int getKey();

private:
#ifdef HAS_NCURSES
    WINDOW* _win = nullptr;
#endif
    bool _initialized = false;
};

}} // namespace matrixcli::tui

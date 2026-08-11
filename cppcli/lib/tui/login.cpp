#include "login.hpp"
#include "screen.hpp"

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif
#include <cctype>

namespace matrixcli { namespace tui {

LoginResult LoginView::run(Screen& screen) {
    LoginResult result;

#ifdef HAS_NCURSES
    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);

    screen.drawTextCentered(2, "Matrix Login");
    screen.drawTextCentered(4, "Homeserver URL:");

    char url_buf[256] = {};
    move(5, 2);
    wgetnstr(stdscr, url_buf, 255);
    result.homeserver = url_buf;

    screen.drawTextCentered(7, "Username:");
    char user_buf[256] = {};
    move(8, 2);
    wgetnstr(stdscr, user_buf, 255);
    result.username = user_buf;

    screen.drawTextCentered(10, "Password:");
    noecho();
    char pass_buf[256] = {};
    move(11, 2);
    wgetnstr(stdscr, pass_buf, 255);
    result.password = pass_buf;
    echo();

    result.success = !result.username.empty() && !result.password.empty();
    if (result.homeserver.empty()) {
        result.homeserver = "https://matrix.org";
    }

    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
#endif

    return result;
}

}} // namespace matrixcli::tui

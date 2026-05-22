#include "help_screen.hpp"

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif
#include <thread>
#include <chrono>

namespace matrixcli { namespace tui {

const char* HelpScreen::helpText() {
    return
        "╔══════════════════════ matrixcli HELP ═══════════════════════════╗\n"
        "║                                                                ║\n"
        "║  NAVIGATION                                                    ║\n"
        "║    Tab          Switch focus (rooms ↔ input)                   ║\n"
        "║    j / ↓        Scroll down (rooms or messages)                ║\n"
        "║    k / ↑        Scroll up                                      ║\n"
        "║    Enter        Select room / Send message                     ║\n"
        "║    PgUp/PgDn    Page through messages                          ║\n"
        "║    Home/End     Top/bottom of message list                     ║\n"
        "║    m            Toggle member list panel                       ║\n"
        "║    Ctrl+f       Fuzzy search rooms                             ║\n"
        "║    ?            Show this help                                 ║\n"
        "║    q            Quit (empty input)                             ║\n"
        "║                                                                ║\n"
        "║  MESSAGE FORMATTING                                            ║\n"
        "║    **bold**     Bold text                                      ║\n"
        "║    *italic*     Italic text                                    ║\n"
        "║    `code`       Inline code                                    ║\n"
        "║    ```          Code block                                     ║\n"
        "║    > quote      Block quote                                    ║\n"
        "║    [text](url)  Link                                           ║\n"
        "║                                                                ║\n"
        "║  SLASH COMMANDS                                                ║\n"
        "║    /me text     Send action (/me waves)                        ║\n"
        "║    /notice txt  Send notice                                    ║\n"
        "║    /join #room  Join a room                                    ║\n"
        "║    /leave       Leave current room                             ║\n"
        "║    /nick name   Change display name                            ║\n"
        "║    /invite @u   Invite user to room                            ║\n"
        "║    /kick @u     Kick user from room                            ║\n"
        "║    /ban @u      Ban user from room                             ║\n"
        "║    /redact      Redact last message                            ║\n"
        "║    /react emoji React to last message                          ║\n"
        "║    /shrug       ¯\\_(ツ)_/¯                                     ║\n"
        "║    /tableflip   (╯°□°)╯︵ ┻━┻                                  ║\n"
        "║                                                                ║\n"
        "║  Press any key to close                                        ║\n"
        "╚════════════════════════════════════════════════════════════════╝";
}

void HelpScreen::show(Screen& screen) {
#ifdef HAS_NCURSES
    int w = screen.width();
    int h = screen.height();
    int bw = std::min(66, w - 2);
    int bh = std::min(36, h - 2);
    int bx = (w - bw) / 2;
    int by = (h - bh) / 2;

    WINDOW* win = newwin(bh, bw, by, bx);
    box(win, 0, 0);
    wattron(win, A_BOLD);
    mvwprintw(win, 0, (bw - 12) / 2, " HELP ");
    wattroff(win, A_BOLD);

    const char* text = helpText();
    int line = 1;
    for (const char* p = text; *p && line < bh - 1; p++) {
        if (*p == '\n') { line++; continue; }
        if (line >= 1 && line < bh - 1) {
            int col = p - text;
            // Find start of current line
            const char* ls = p;
            while (ls > text && *(ls - 1) != '\n') ls--;
            int x = p - ls + 1;
            if (x < bw - 2) mvwaddch(win, line, x, *p);
        }
    }

    wrefresh(win);
    while (screen.getKey() == -1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Consume the key
    while (screen.getKey() != -1) {}
    delwin(win);
    werase(stdscr);
#else
    (void)screen;
#endif
}

}} // namespace matrixcli::tui

// src/ascii_ui.cpp — ASCII-drawn client interface for the CLI (not the TUI).
//
// `progressive-cli ui` draws a chat-client-like layout with plain characters:
// a header, a left panel with the room list, the open room's messages in
// the center and the member list on the right, panels separated by pipes.
// It is a REPL: every command executes and the whole frame is redrawn —
// no auto-updates, no raw terminal mode (works in any terminal, scrolls
// like a normal CLI program).
#include "ascii_ui.hpp"
#include "ascii_state.hpp"
#include "commands.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/string_utils.hpp"
#include "agent_tools.hpp"
#include <cstdlib>
#include <glob.h>
#include <poll.h>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "cli/args.hpp"
#include "pcore.hpp"
#include "globals.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

#include "ascii_ui_impl.hpp"

namespace matrixcli {

void printAbout(const std::string& proxyLabel, const std::string& accountLabel) {
    const char* X = "\x1b[0m";
    std::cout << "\x1b[1;31mprogressive-cli\x1b[0m"
              << " — the Matrix chat client and coding agent, designed to work in the terminal\n";
    std::cout << "(c) Progressive Chat contributors\n\n";
    const char* BR = "\x1b[1;31m";  // bold red
    std::cout << BR << "       /\n";
    std::cout << "      /\n";
    std::cout << "     /\n";
    std::cout << "    /\n";
    std::cout << " /\\/\n";
    std::cout << "/" << X << "\n";
    std::cout << "      — chat progress, always increasing —\n\n";
    std::string ver = cli::versionString();
    // "progressive-cli v0.5.2" → just the number part.
    size_t v = ver.rfind('v');
    std::cout << "Version: " << (v == std::string::npos ? ver : ver.substr(v + 1))
              << "\n";
    std::cout << "License: AGPL-3.0\n";
    std::cout << "https://github.com/progressive-chat/progressive-cli\n";
    (void)proxyLabel; (void)accountLabel;
}



// ---- Mini line editor with the command history ----
// The terminal is switched to raw mode so the arrow keys arrive as escape
// sequences; the line is rendered by us. Restores the terminal on exit.
bool readLineWithHistory(std::vector<std::string>& history,
                         const std::string& prompt, std::string& out) {
    out.clear();
    bool isTty = isatty(STDIN_FILENO);
    struct termios oldt{};
    if (isTty && tcgetattr(STDIN_FILENO, &oldt) != 0) isTty = false;
    struct termios raw = oldt;
    if (isTty) {
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    auto restore = [&]() {
        if (isTty) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    };
    if (!isTty) {
        // Piped input: no arrows, plain getline.
        std::cout << prompt << std::flush;
        if (!std::getline(std::cin, out)) return false;
        return true;
    }
    std::cout << prompt << std::flush;
    size_t hpos = history.size();
    auto render = [&]() {
        std::cout << "\r\033[K" << prompt << out << std::flush;
    };
    bool ok = true;
    for (;;) {
        int c = getchar();
        if (c == EOF) {  // the Ctrl+C EINTR (or the stream end)
            ok = false;
            break;
        }
        if (c == '\x03') {  // Ctrl+C
            ok = false;
            break;
        }
        if (c == '\n') {    // Enter
            std::cout << "\n";
            break;
        }
        if (c == 0x7f || c == 0x08) {  // Backspace
            if (!out.empty()) { out.pop_back(); render(); }
            continue;
        }
        if (c == '\x1b') {   // Escape sequences (arrow keys)
            int a = getchar();
            if (a != '[') continue;
            int b = getchar();
            if (b == 'A') {   // Up
                if (hpos > 0) {
                    hpos--;
                    out = history[hpos];
                    render();
                }
            } else if (b == 'B') {  // Down
                if (hpos < history.size()) {
                    hpos++;
                    out = (hpos < history.size()) ? history[hpos] : "";
                    render();
                }
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            out += static_cast<char>(c);
            render();
        }
    }
    restore();
    if (ok && !out.empty() && (history.empty() || history.back() != out)) {
        history.push_back(out);
    }
    return ok;
}

// One-shot 'progressive-cli about' — the about screen without the ui.
int cmdAbout(const cli::Args&) {
    // No session/network init — the about screen must be clean and instant.
    printAbout("", "");
    return 0;
}

} // namespace matrixcli

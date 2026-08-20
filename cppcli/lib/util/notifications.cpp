#include "notifications.hpp"
#include "logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace matrixcli { namespace util {

namespace {

// The shell-safe single-quote quoting for the notify-send arguments.
std::string shQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// One line, no control characters, a sane length limit — notification
// daemons and the shell both hate the rest.
std::string clean(const std::string& s, size_t maxLen) {
    std::string out;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        out += c;
        if (out.size() >= maxLen) { out += "..."; break; }
    }
    return out;
}

} // namespace

bool Notifications::available() {
    return system("which notify-send >/dev/null 2>&1") == 0 ||
           system("which qdbus6 >/dev/null 2>&1 || which qdbus >/dev/null 2>&1") == 0;
}

void Notifications::send(const std::string& title, const std::string& body) {
    // The terminal bell always works.
    bell();

    const std::string t = clean(title, 120);
    const std::string b = clean(body, 200);

    // Prefer notify-send (libnotify, any desktop); fall back to the
    // org.freedesktop.Notifications D-Bus service directly (KDE Plasma
    // runs it as part of plasmashell, so qdbus6 reaches the same tray).
    if (system("which notify-send >/dev/null 2>&1") == 0) {
        std::string cmd = "notify-send " + shQuote(t) + " " + shQuote(b) +
                          " --app-name=progressive-cli "
                          "--icon=dialog-information &";
        if (system(cmd.c_str()) != 0) {
            Logger::instance().error("notify-send failed");
        }
        return;
    }
    const char* qdbus = system("which qdbus6 >/dev/null 2>&1") == 0
                            ? "qdbus6" : "qdbus";
    if (qdbus != nullptr) {
        std::string cmd = std::string(qdbus) +
                          " org.freedesktop.Notifications "
                          "/org/freedesktop/Notifications "
                          "org.freedesktop.Notifications.Notify "
                          "progressive-cli 0 dialog-information " +
                          shQuote(t) + " " + shQuote(b) + " &";
        system(cmd.c_str());
    }
}

void Notifications::bell() {
    // Terminal bell character
    printf("\a");
    fflush(stdout);
}

// ── The forwarding service ──────────────────────────────────────────
// Run it in the session that owns the desktop — possibly as another
// Linux user ("sudo -u <desktop-user> matrixcli notify daemon"): it
// listens on a TCP port and shows every received notification in THAT
// session's notification daemon. The wire format is a single line per
// notification: "title\tbody\n" (both cleaned, length-capped).

#ifndef _WIN32

namespace {

constexpr int kDefaultNotifyPort = 27430;

} // namespace

void Notifications::runDaemon(const std::string& bind, int port) {
    const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::printf("notify daemon: socket: %s\n", std::strerror(errno));
        return;
    }
    int yes = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind.empty() || bind == "127.0.0.1") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (::inet_pton(AF_INET, bind.c_str(), &addr.sin_addr) != 1) {
        std::printf("notify daemon: bad bind address '%s'\n", bind.c_str());
        ::close(listenFd);
        return;
    }
    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listenFd, 4) != 0) {
        std::printf("notify daemon: bind/listen on %s:%d: %s\n",
                    bind.empty() ? "127.0.0.1" : bind.c_str(), port,
                    std::strerror(errno));
        ::close(listenFd);
        return;
    }

    std::printf("notify daemon: listening on %s:%d — send notifications "
                "with 'notify host %s:%d' from any machine/user\n",
                bind.empty() ? "127.0.0.1" : bind.c_str(), port,
                bind.empty() ? "127.0.0.1" : bind.c_str(), port);
    std::fflush(stdout);

    for (;;) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int fd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer),
                                &plen);
        if (fd < 0) continue;

        std::string line;
        char buf[256];
        ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
            line.append(buf, static_cast<size_t>(n));
            if (line.size() > 4096) break;
        }
        ::close(fd);

        const auto nl = line.find('\n');
        if (nl != std::string::npos) line = line.substr(0, nl);
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string title = line.substr(0, tab);
        const std::string body = line.substr(tab + 1);
        std::printf("notify daemon: %s: %s\n", title.c_str(), body.c_str());
        std::fflush(stdout);
        send(title, body);
    }
}

bool Notifications::sendToDaemon(const std::string& host, int port,
                                 const std::string& title,
                                 const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    const int ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr));
    if (ok != 0) {
        ::close(fd);
        return false;
    }

    std::string line = clean(title, 120) + "\t" + clean(body, 200) + "\n";
    const bool sent = ::send(fd, line.data(), line.size(), 0) ==
                      static_cast<ssize_t>(line.size());
    ::close(fd);
    return sent;
}

#else // _WIN32

void Notifications::runDaemon(const std::string&, int) {
    std::printf("notify daemon: not supported on Windows\n");
}

bool Notifications::sendToDaemon(const std::string&, int,
                                 const std::string&, const std::string&) {
    return false;
}

#endif

}} // namespace matrixcli::util
#pragma once

#include <string>
#include <functional>

namespace matrixcli { namespace tui {

class Screen;

struct LoginResult {
    bool success = false;
    std::string username;
    std::string password;
    std::string homeserver;
    // The connection type: "" = direct, "tor" = SOCKS5 127.0.0.1:9050,
    // "i2p" = HTTP 127.0.0.1:4444, "yggdrasil" = the mesh (direct IPv6),
    // "custom <host:port>" = SOCKS5-hostname.
    std::string connection;
};

class LoginView {
public:
    LoginResult run(Screen& screen);
};

}} // namespace matrixcli::tui

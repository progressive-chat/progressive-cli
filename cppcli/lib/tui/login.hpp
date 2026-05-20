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
};

class LoginView {
public:
    LoginResult run(Screen& screen);
};

}} // namespace matrixcli::tui

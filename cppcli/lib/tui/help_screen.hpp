#pragma once

#include "screen.hpp"
#include <string>
#include <functional>

namespace matrixcli { namespace tui {

class HelpScreen {
public:
    static void show(Screen& screen);

private:
    static const char* helpText();
};

}} // namespace matrixcli::tui

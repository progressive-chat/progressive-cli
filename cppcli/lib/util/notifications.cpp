#include "notifications.hpp"
#include "logger.hpp"

#include <cstdlib>
#include <thread>

namespace matrixcli { namespace util {

bool Notifications::available() {
    return system("which notify-send >/dev/null 2>&1") == 0;
}

void Notifications::send(const std::string& title, const std::string& body) {
    // Terminal bell always works
    bell();

    // Try notify-send for desktop notification
    if (available()) {
        std::string cmd = "notify-send '" + title + "' '" + body +
                          "' --app-name=matrixcli --icon=dialog-information &";
        system(cmd.c_str());
    }
}

void Notifications::bell() {
    // Terminal bell character
    printf("\a");
    fflush(stdout);
}

}} // namespace matrixcli::util

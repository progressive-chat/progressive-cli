#pragma once

#include <string>

namespace matrixcli { namespace util {

class Notifications {
public:
    static void send(const std::string& title, const std::string& body);
    static void bell();
    static bool available();
};

}} // namespace matrixcli::util

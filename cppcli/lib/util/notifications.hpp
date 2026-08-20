#pragma once

#include <string>

namespace matrixcli { namespace util {

class Notifications {
public:
    // A native desktop notification in THIS session: notify-send first,
    // a direct qdbus6/qdbus call as the fallback, the terminal bell always.
    static void send(const std::string& title, const std::string& body);

    // The forwarding service (run it in the session that owns the desktop,
    // possibly as another Linux user): listens on a TCP port and shows the
    // received notifications locally. Blocking; prints on stdout.
    static void runDaemon(const std::string& bind, int port);

    // Send one notification to a running runDaemon service. Returns false
    // when the service is unreachable.
    static bool sendToDaemon(const std::string& host, int port,
                             const std::string& title,
                             const std::string& body);

    static void bell();
    static bool available();
};

}} // namespace matrixcli::util
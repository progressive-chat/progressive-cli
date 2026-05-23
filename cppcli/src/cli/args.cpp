#include "args.hpp"

#include <iostream>
#include <cstring>

namespace matrixcli { namespace cli {

Args parseArgs(int argc, char* argv[]) {
    Args result;

    if (argc < 2) {
        return result;
    }

    result.command = argv[1];
    if (result.command.starts_with("--")) {
        result.options[result.command.substr(2)] = "true";
        result.command.clear();
    }

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.starts_with("--")) {
            std::string key = arg.substr(2);
            std::string value = "true";

            auto eq = key.find('=');
            if (eq != std::string::npos) {
                value = key.substr(eq + 1);
                key = key.substr(0, eq);
            } else if (i + 1 < argc && !std::string(argv[i + 1]).starts_with("--")) {
                value = argv[++i];
            }

            result.options[key] = value;
        } else if (arg.starts_with("-") && arg.size() == 2) {
            std::string key(1, arg[1]);
            std::string value = "true";

            if (i + 1 < argc && !std::string(argv[i + 1]).starts_with("-")) {
                value = argv[++i];
            }

            result.options[key] = value;
        } else {
            result.positional.push_back(arg);
        }
    }

    return result;
}

void printUsage() {
    std::cout << "matrixcli - A Matrix CLI client\n\n"
              << "Usage: matrixcli [command] [options]\n\n"
              << "Commands:\n"
              << "  login         Login to a Matrix homeserver\n"
              << "  status        Show login status and sync token\n"
              << "  rooms         List joined rooms\n"
              << "  view          View room messages (offline from cache)\n"
              << "  send          Send a message to a room\n"
              << "  react         Add a reaction to a message\n"
              << "  vote          Vote in a poll\n"
              << "  serve         Start the built-in HTTP API server\n"
              << "  demo          Start API server in demo mode (no Matrix account)\n"
              << "  tui           Launch the interactive terminal UI\n"
              << "\n"
              << "Examples:\n"
              << "  matrixcli login --homeserver https://matrix.org --username @me:matrix.org --password s3cret\n"
              << "  matrixcli rooms\n"
              << "  matrixcli view \"#general:matrix.org\" 50\n"
              << "  matrixcli send \"#general:matrix.org\" \"Hello from CLI!\"\n"
              << "  matrixcli demo --port=9999\n"
              << "  matrixcli serve --port=29325\n"
              << "\n"
              << "Options for serve/demo:\n"
              << "  --port, -p    Port to listen on (default: 8080)\n"
              << "  --host, -h    Host to bind to (default: 127.0.0.1)\n"
              << "  --demo        Enable demo mode with fake data\n"
              << "\n"
              << "Options for login:\n"
              << "  --homeserver  Homeserver URL (e.g., https://matrix.org)\n"
              << "  --username    Matrix username (@user:server)\n"
              << "  --password    Password\n"
              << "  --token       Login with access token\n"
              << "\n"
              << "General options:\n"
              << "  --help        Show this help\n"
              << "  --version     Show version\n"
              << std::endl;
}

void printVersion() {
    std::cout << "matrixcli v0.1.0" << std::endl;
}

}} // namespace matrixcli::cli

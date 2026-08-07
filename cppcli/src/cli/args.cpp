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
              << "  e2ee          E2EE status and key management (status/upload/fallback)\n"
              << "  backup        Key backup: create/upload/restore/delete (--recovery-key)\n"
              << "  crosssign     Cross-signing: setup/reset (--password for UIA)\n"
              << "  ssss          Secret storage: upload/retrieve (--recovery-key)\n"
              << "  verify        SAS-verify a device: verify <user> --device <id> [--confirm]\n"
              << "  sync          One-shot sync into the offline cache\n"
              << "  devices       Delete a device: devices delete <id> --password <pw>\n"
              << "  kick/ban/unban  Moderator actions: kick <room> <@user> [--reason r]\n"
              << "  profile       User profile: profile <@user>\n"
              << "  members       Room members: members <room>\n"
              << "  threads       Room threads: threads <room> [--limit N]\n"
              << "  search-public Public room directory: search-public <query> [--server hs]\n"
              << "  rooms         List joined rooms\n"
              << "  view          View room messages (offline from cache)\n"
              << "  send          Send a message to a room\n"
              << "  reply         Reply to a message\n"
              << "  edit          Edit a message\n"
              << "  redact        Delete (redact) a message\n"
              << "  react         Add a reaction to a message\n"
              << "  vote          Vote in a poll\n"
              << "  search        Full-text search in cached messages\n"
              << "  read          Mark room as read\n"
              << "  topic         Set room topic\n"
              << "  roomname      Set room name\n"
              << "  avatar        Set room avatar\n"
              << "  knock         Knock on a room\n"
              << "  info          Show room info\n"
              << "  config        Show/edit client config\n"
              << "  notifications  Notification settings\n"
              << "  setup         Interactive setup wizard\n"
              << "  irc           IRC client (connect/join/msg/leave/whois/names)\n"
              << "  lemmy         Lemmy client (login/posts/post/upvote/comments)\n"
              << "  td            Telegram via TDLib (login/chats/msg/history)\n"
              << "  dc            DeltaChat bridge\n"
              << "  serve         Start the built-in HTTP API server\n"
              << "  demo          Start API server in demo mode (no Matrix account)\n"
              << "  tui           Launch the terminal UI (optional add-on)\n"
              << "  completion    Generate shell completion (bash/zsh/fish)\n"
              << "  help          Show this help\n"
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
              << "  --json        Pipe-friendly JSON output\n"
              << "  --debug       Show raw event data\n"
              << "  --ts          Show relative timestamps (3m ago)\n"
              << "  --ids         Show event IDs\n"
              << "\nQuick start:\n"
              << "  matrixcli demo populate          # offline demo\n"
              << "  matrixcli rooms                  # list rooms\n"
              << "  matrixcli view \"#general\" --ts  # view messages\n"
              << "  matrixcli demo --port=8080       # API server\n"
              << "  matrixcli tui                    # terminal UI\n"
              << "\nShell completions:\n"
              << "  source <(matrixcli completion bash)\n"
              << std::endl;
}

void printVersion() {
    std::cout << "matrixcli v0.1.0" << std::endl;
}

}} // namespace matrixcli::cli

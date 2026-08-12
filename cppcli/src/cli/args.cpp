#include "args.hpp"

#include <iostream>
#include <cstring>
#include <unordered_set>

namespace matrixcli { namespace cli {

// Flags that never take a value — the NEXT argument stays positional.
// Without this, "--static #general" swallowed the room id as the flag's
// value and the ui/demo drew the default room.
static const std::unordered_set<std::string> kNoValueFlags = {
    "static", "once", "print", "json", "confirm", "debug", "ts", "ids",
    "expand", "verbose", "no-replies", "no-filter", "all", "interactive",
    "help", "version", "cli", "ui", "ascii", "populate", "mobile",
    "no-mouse", "mouse",
};

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
            } else if (i + 1 < argc && !std::string(argv[i + 1]).starts_with("--") &&
                       !kNoValueFlags.count(key)) {
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
              << "  accounts      Logged-in accounts: accounts [--all] [--json] | --hide <mxid> | --show <mxid>\n"
              << "  agent         Agentic loop with Matrix tools: agent <task> [--room X] [--token t]\n"
              << "  attach        Send a file: attach <room> <file> [--caption text]\n"
              << "  avatar        Set room avatar\n"
              << "  backup        Key backup: create/upload/restore/delete (--recovery-key)\n"
              << "  completion    Generate shell completion (bash/zsh/fish)\n"
              << "  config        Show/edit client config\n"
              << "  crosssign     Cross-signing: setup/reset (--password for UIA)\n"
              << "  dc            DeltaChat bridge\n"
              << "  demo          Offline demo: REPL | --cli one-shot | --ui [--static] ASCII interface\n"
              << "  devices       Delete a device: devices delete <id> --password <pw>\n"
              << "  e2ee          E2EE status and key management (status/upload/fallback)\n"
              << "  edit          Edit a message\n"
              << "  filter        Permanent view filters: filter --senders @u [--room X] | --hide @u [--room X] | status | clear\n"
              << "  help          Show this help\n"
              << "  info          Show room info\n"
              << "  irc           IRC client (connect/join/msg/leave/whois/names)\n"
              << "  kick/ban/unban  Moderator actions: kick <room> <@user> [--reason r]\n"
              << "  knock         Knock on a room\n"
              << "  lemmy         Lemmy client (login/posts/post/upvote/comments)\n"
              << "  llm           LLM completion: llm <prompt> [--provider openai|anthropic] [--token t] [--model m]\n"
              << "  login         Login to a Matrix homeserver\n"
              << "  markdown      Render markdown to HTML: markdown <text> | echo <text> | matrixcli markdown\n"
              << "  members       Room members: members <room>\n"
              << "  notifications  Notification settings\n"
              << "  profile       User profile: profile <@user>\n"
              << "  proxy         Tor/I2P proxy: on|off|status (--host --port [--type socks5h|socks5|http])\n"
              << "  react         Add a reaction to a message\n"
              << "  read          Mark room as read\n"
              << "  redact        Delete (redact) a message\n"
              << "  reply         Reply to a message\n"
              << "  roomname      Set room name\n"
              << "  rooms         List joined rooms\n"
              << "  search        Full-text search in cached messages\n"
              << "  search-public Public room directory: search-public <query> [--server hs]\n"
              << "  send          Send a message to a room\n"
              << "  serve         Start the built-in HTTP API server\n"
              << "  setup         Interactive setup wizard\n"
              << "  ssss          Secret storage: upload/retrieve (--recovery-key)\n"
              << "  status        Show login status and sync token\n"
              << "  sync          One-shot sync into the offline cache\n"
              << "  td            Telegram via TDLib (login/chats/msg/history)\n"
              << "  threads       Room threads: threads <room> [--limit N]\n"
              << "  topic         Set room topic\n"
              << "  tui           Launch the terminal UI (optional add-on)\n"
              << "  typing        Who is typing: typing <room>\n"
              << "  ui            ASCII-drawn client interface (rooms | chat | members)\n"
              << "  verify        SAS-verify a device: verify <user> --device <id> [--confirm]\n"
              << "  verify-wait   Accept an incoming SAS request: verify-wait [--confirm] [--timeout s]\n"
              << "  view          View room messages (offline from cache): view <room> [limit] [--senders @u] [--hide @u] [--replies N]\n"
              << "  vote          Vote in a poll\n"
              << "\n"
              << "Examples:\n"
              << "  matrixcli login --homeserver https://matrix.org --username @me:matrix.org --password s3cret\n"
              << "  matrixcli rooms\n"
              << "  matrixcli view \"#general:matrix.org\" 50\n"
              << "  matrixcli send \"#general:matrix.org\" \"Hello from CLI!\"\n"
              << "  matrixcli demo                        # interactive demo session\n"
              << "  matrixcli serve --port=29325\n"
              << "\n"
              << "Options for serve:\n"
              << "  --port, -p    Port to listen on (default: 8080)\n"
              << "  --host, -h    Host to bind to (default: 127.0.0.1)\n"
              << "  --demo        Serve the web demo (API server with fake data)\n"
              << "\n"
              << "Options for ui / demo --ui (view flags, can be combined):\n"
              << "  --ids         show the event id (\u2039$abc\u203a) next to messages\n"
              << "  --time-full   message time with seconds (HH:MM:SS)\n"
              << "  --limit N     only the last N events of the room\n"
              << "  --right members|threads|list|thread  right panel mode\n"
              << "  --thread-root <id>  the thread to show (with --right thread)\n"
              << "  --static      draw the frame once and exit (pipe-friendly)\n"
              << "  --rows N      frame height in rows (0/omit = fit terminal)\n"
              << "  --scroll N    viewport offset in the room list (with --rows)\n"
              << "  --scroll-left N  scroll ONLY the rooms list (desktop)\n"
              << "  --mobile      smartphone layout: stacked sections (portrait)\n"
              << "  --panel-left <off|on|width>   temporary left panel size\n"
              << "  --panel-right <off|on|width>  temporary right panel size\n"
              << "  --panel-auto on|off  temporary auto-sizing toggle\n"
              << "  --members horizontal|list|auto  temporary member list layout\n"
              << "  --media       with --static: also render the room's image previews\n"
              << "\n"
              << "Options for login:\n"
              << "  --homeserver  Homeserver URL (e.g., https://matrix.org)\n"
              << "  --username    Username localpart (e.g. me) — @me:server also accepted\n"
              << "  --mxid        Alias for --username, expects full @user:server\n"
              << "  --password    Password\n"
              << "  --token       Login with access token\n"
              << "  --register    Register a new account (with --username/--password)\n"
              << "  --reg-token   Registration token (m.login.registration_token)\n"
              << "  --interactive Prompt for missing username/password (password hidden)\n"
              << "\n"
              << "General options:\n"
              << "  --help        Show this help\n"
              << "  --version     Show version\n"
              << "\n"
              << "Project: https://github.com/progressive-chat/progressive-cli\n"
              << "  --json        Pipe-friendly JSON output\n"
              << "  --debug       Show raw event data\n"
              << "  --ts          Show relative timestamps (3m ago)\n"
              << "  --ids         Show event IDs\n"
              << "\nQuick start:\n"
              << "  matrixcli demo populate          # offline demo\n"
              << "  matrixcli rooms                  # list rooms\n"
              << "  matrixcli view \"#general\" --ts  # view messages\n"
              << "  matrixcli serve --demo            # web demo (HTTP API server)\n"
              << "  matrixcli tui                    # terminal UI\n"
              << "\nShell completions:\n"
              << "  source <(matrixcli completion bash)\n"
              << std::endl;
}

void printVersion() {
    std::cout << "matrixcli v0.1.0" << std::endl;
}

}} // namespace matrixcli::cli

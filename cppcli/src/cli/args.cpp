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
    "no-mouse", "mouse", "agent",
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
    std::cout << "progressive-cli - the Matrix chat client and coding agent, designed to work in the terminal\n\n"
              << "Usage: progressive-cli [command] [options]\n\n";

    std::cout << "The Matrix client\n"
              << "  accounts      Logged-in accounts: accounts [--all] [--json] | --hide <mxid> | --show <mxid>\n"
              << "  attach        Send a file: attach <room> <file> [--caption text]\n"
              << "  avatar        Set the room avatar\n"
              << "  backup        Key backup: create/upload/restore/delete (--recovery-key)\n"
              << "  call          VoIP signaling: call <@user> | answer <id> | hangup <id> | status | wait\n"
              << "  crosssign     Cross-signing: setup/reset (--password for UIA)\n"
              << "  demo          Offline demo: the REPL | --cli one-shot | --ui [--static] the ASCII interface\n"
              << "  devices       Delete a device: devices delete <id> --password <pw>\n"
              << "  e2ee          E2EE status and the key management (status/upload/fallback)\n"
              << "  edit          Edit a message\n"
              << "  filter        Permanent view filters: --senders @u [--room X] | --hide @u [--room X] | status | clear\n"
              << "  info          Show the room info\n"
              << "  invite        Invite a user: invite <room> <@user> [--reason r]\n"
              << "  kick/ban/unban  The moderator actions: kick|ban|unban <room> <@user> [--reason r]\n"
              << "  knock         Knock on a room\n"
              << "  markdown      Render markdown to HTML: markdown <text> | echo <text> | progressive-cli markdown\n"
              << "  members       The room members: members <room>\n"
              << "  notifications Notification settings: notifications (on|off)\n"
              << "  profile       The user profile: profile <@user>\n"
              << "  react         Add a reaction to a message\n"
              << "  read          Mark the room as read\n"
              << "  receipts      The per-room read receipts: receipts [<room>] [on|off]\n"
              << "  redact        Delete (redact) a message\n"
              << "  reply         Reply to a message\n"
              << "  roomname      Set the room name\n"
              << "  rooms         List the joined rooms\n"
              << "  search        The full-text search in the cached messages\n"
              << "  search-public The public room directory: search-public <query> [--server hs]\n"
              << "  send          Send a message to a room\n"
              << "  ssss          The secret storage: upload/retrieve (--recovery-key)\n"
              << "  status        Show the login status and the sync token\n"
              << "  sync          One-shot sync into the offline cache\n"
              << "  threads       The room threads: threads <room> [--limit N]\n"
              << "  topic         Set the room topic\n"
              << "  tui           Launch the terminal UI (the optional add-on)\n"
              << "  typing        Who is typing: typing <room>\n"
              << "  ui            The ASCII-drawn client (rooms | chat | members)\n"
              << "  spaces        The spaces in the cache: spaces [--json] (the ui also has 'space <name>' to filter the room list)\n"
              << "  verify        SAS-verify a device: verify <user> --device <id> [--confirm]\n"
              << "  verify-wait   Accept an incoming SAS request: verify-wait [--confirm] [--timeout s]\n"
              << "  view          View the room messages (offline, the cache): view <room> [limit] [--senders @u] [--hide @u]\n"
              << "  vote          Vote in a poll\n\n";

    std::cout << "The LLM and the agents\n"
              << "  llm           The LLM completion / the conversations: llm <prompt> | llm chat | llm continue | llm sessions | llm resume <N>\n"
              << "  agent         The agentic loop with the Matrix tools: agent <task> [--room X]\n"
              << "  agent-code    The local coding agent: agent-code <prompt> [--trust allow|ask|deny]\n\n";

    std::cout << "The setup and the infrastructure\n"
              << "  completion    Generate the shell completion (bash/zsh/fish)\n"
              << "  config        Show/edit the client config\n"
              << "  help          Show this help\n"
              << "  login         Login to a Matrix homeserver\n"
              << "  proxy         The Tor/I2P proxy: on|off|status (--host --port [--type socks5h|socks5|http])\n"
              << "  serve         Start the built-in HTTP API server\n"
              << "  setup         The interactive setup wizard\n\n";

    std::cout << "The bridges (experimental)\n"
              << "  dc            The DeltaChat bridge\n"
              << "  irc           The IRC client (connect/join/msg/leave/whois/names)\n"
              << "  lemmy         The Lemmy client (login/posts/post/upvote/comments)\n"
              << "  td            Telegram via TDLib (login/chats/msg/history)\n\n";

    std::cout << "Examples:\n"
              << "  progressive-cli login --homeserver https://matrix.org --username @me:matrix.org --password s3cret\n"
              << "  progressive-cli rooms\n"
              << "  progressive-cli view \"#general:matrix.org\" 50\n"
              << "  progressive-cli send \"#general:matrix.org\" \"Hello from CLI!\"\n"
              << "  progressive-cli llm \"explain this code\" --rich\n"
              << "  progressive-cli demo                        # the interactive demo session\n"
              << "  progressive-cli serve --port=29325\n"
              << "\n"
              << "The detailed flags: progressive-cli <command> --help\n";
}

void printVersion() {
    std::cout << "progressive-cli v0.5.2" << std::endl;
}

}} // namespace matrixcli::cli

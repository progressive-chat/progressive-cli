#include "args.hpp"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unordered_set>
#include <unistd.h>

#include "../lib/util/string_utils.hpp"

namespace matrixcli { namespace cli {

// Flags that never take a value — the NEXT argument stays positional.
// Without this, "--static #general" swallowed the room id as the flag's
// value and the ui/demo drew the default room.
static const std::unordered_set<std::string> kNoValueFlags = {
    "static", "once", "print", "json", "confirm", "debug", "ts", "ids",
    "expand", "verbose", "no-replies", "no-filter", "all", "interactive",
    "help", "version", "cli", "ui", "ascii", "populate", "mobile",
    "no-mouse", "mouse", "agent", "disable-formatting",
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

// Colour/formatting in the help output is opt-out. Plain when: the
// --disable-formatting flag is given (anywhere on the line), stdout is
// not a terminal (pipes, scripts), NO_COLOR is set, or TERM=dumb.
bool wantFormatting(const Args& args) {
    if (args.options.contains("disable-formatting")) {
        return false;
    }
    if (!isatty(STDOUT_FILENO)) {
        return false;
    }
    const char* noColor = getenv("NO_COLOR");
    if (noColor && *noColor) {
        return false;
    }
    const char* term = getenv("TERM");
    if (!term || std::string(term) == "dumb") {
        return false;
    }
    return true;
}

void printUsage(const Args& args) {
    const bool fmt = wantFormatting(args);
    // Plain mode renders the exact same bytes as before: the colour
    // tokens reduce to empty strings.
    const std::string head = fmt ? std::string(ANSI_BOLD) + ANSI_CYAN : "";
    const std::string usage = fmt ? std::string(ANSI_BOLD) + ANSI_YELLOW : "";
    const std::string bold = fmt ? std::string(ANSI_BOLD) : "";
    const std::string dim = fmt ? std::string(ANSI_DIM) : "";
    const std::string demoCol = fmt ? std::string(ANSI_BOLD) + "\033[37m" : "";
    const std::string rst = fmt ? "\033[0m" : "";

    std::cout << head << "progressive-cli - the Matrix chat client and coding agent, designed to work in the terminal" << rst << "\n\n"
              << usage << "Usage: progressive-cli [command] [options]" << rst << "\n\n";

    std::cout << bold << "The Matrix client" << rst << "\n"
              << "  " << demoCol << "accounts" << rst << "      Logged-in accounts: accounts [--all] [--json] | --hide <mxid> | --show <mxid>\n"
              << "  attach        Send a file: attach <room> <file> [--caption text] [--chunks N]\n"
              << "  avatar        Set the room avatar\n"
              << "  " << demoCol << "backup" << rst << "        Key backup: create/upload/restore/delete (--recovery-key)\n"
              << "  call          VoIP signaling: call <@user> | answer <id> | hangup <id> | status | wait\n"
              << "  crosssign     Cross-signing: setup/reset (--password for UIA)\n"
              << "  demo          Offline demo: the REPL | --cli one-shot | --ui [--static] the ASCII interface | demo <room> [info|view|power] — showcase a room | demo " << demoCol << "markdown" << rst << "|" << demoCol << "vote" << rst << "|" << demoCol << "accounts" << rst << "|" << demoCol << "edit" << rst << "|" << demoCol << "report" << rst << "|" << demoCol << "members" << rst << "|" << demoCol << "typing" << rst << "|" << demoCol << "topic" << rst << "|" << demoCol << "threads" << rst << "|" << demoCol << "config" << rst << "|" << demoCol << "backup" << rst << "\n"
              << "  devices       Delete a device: devices delete <id> --password <pw>\n"
              << "  e2ee          E2EE status and the key management (status/upload/fallback)\n"
              << "  " << demoCol << "edit" << rst << "          Edit a message\n"
              << "  filter        Permanent view filters: --senders @u [--room X] | --hide @u [--room X] | status | clear\n"
              << "  info          Show the room info\n"
              << "  invite        Invite a user: invite <room> <@user> [--reason r]\n"
              << "  kick/ban/unban  The moderator actions: kick|ban|unban <room> <@user> [--reason r]\n"
              << "  " << demoCol << "report" << rst << "        Report a message to the homeserver admin: report <room> <event_id> [--reason r] [--score N]\n"
              << "  knock         Knock on a room\n"
              << "  " << demoCol << "markdown" << rst << "      Render markdown to HTML: markdown <text> | echo <text> | progressive-cli markdown\n"
              << "  " << demoCol << "members" << rst << "       The room members: members <room>\n"
              << "  power         Room power levels / permissions: power <room>\n"
              << "  notifications Notification settings: notifications (on|off)\n"
              << "  notify        Native desktop notification (KDE Plasma etc): notify test [text] | notify last | notify on|off | notify daemon [--port N] | notify host <ip[:port]>|off\n"
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
              << "  " << demoCol << "threads" << rst << "       The room threads: threads <room> [--limit N]\n"
              << "  " << demoCol << "topic" << rst << "         Set the room topic\n"
              << "  tui           Launch the terminal UI (the optional add-on)\n"
              << "  " << demoCol << "typing" << rst << "        Who is typing: typing <room>\n"
              << "  ui            The ASCII-drawn client (rooms | chat | members)\n"
              << "  spaces        The spaces in the cache: spaces [--json] (the ui also has 'space <name>' to filter the room list)\n"
              << "  verify        SAS-verify a device: verify <user> --device <id> [--confirm]\n"
              << "  verify-wait   Accept an incoming SAS request: verify-wait [--confirm] [--timeout s]\n"
              << "  view          View the room messages (offline, the cache): view <room> [limit] [--senders @u] [--hide @u]\n"
              << "  link          Room event permalink: link <room> [last|first|N|-N] [--via N] (no args: last active room)\n"
              << "  permalink      Alias of link (room event permalink): permalink <room> [last|first|N|-N] [--via N] (no args: last active room)\n"
              << "  " << demoCol << "vote" << rst << "          Vote in a poll\n\n";

    std::cout << bold << "The LLM and the agents" << rst << "\n"
              << "  llm           The LLM completion / the conversations: llm <prompt> | llm chat | llm continue | llm sessions | llm resume <N>\n"
              << "  agent         The agentic loop with the Matrix tools: agent <task> [--room X]\n"
              << "  agent-code    The local coding agent: agent-code <prompt> [--trust allow|ask|deny]\n\n";

    std::cout << bold << "The setup and the infrastructure" << rst << "\n"
              << "  completion    Generate the shell completion (bash/zsh/fish)\n"
              << "  " << demoCol << "config" << rst << "        Show/edit the client config\n"
              << "  help          Show this help\n"
              << "  login         Login to a Matrix homeserver\n"
              << "  proxy         The Tor/I2P proxy: on|off|status (--host --port [--type socks5h|socks5|http])\n"
              << "  serve         Start the built-in HTTP API server\n"
              << "  setup         The interactive setup wizard\n\n";

    std::cout << bold << "The bridges (experimental)" << rst << "\n"
              << "  dc            The DeltaChat bridge\n"
              << "  irc           The IRC client (connect/join/msg/leave/whois/names)\n"
              << "  lemmy         The Lemmy client (login/posts/post/upvote/comments)\n"
              << "  td            Telegram via TDLib (login/chats/msg/history)\n\n";

    std::cout << bold << "The markdown rendering (demo markdown)" << rst << "\n"
              << "  bold, italic, inline code and [links](url) as OSC 8 hyperlinks, plus\n"
              << "  headers, lists, - [x] checkboxes, quotes and fenced code. Terminals\n"
              << "  have a fixed cell grid, no font scaling exists: headers render bold.\n"
              << "  Bold/italic need font faces (often missing on phones). OSC 8 links\n"
              << "  open with Ctrl+Click; in Konsole edit the profile: Mouse tab > tick\n"
              << "  'Allow escape sequences for links' (+ 'Open files/links by direct\n"
              << "  click' for tap-to-open), and have a working default browser (gio mime\n"
              << "  x-scheme-handler/https shows what KIO uses). The right-click Open\n"
              << "  Link item for hidden links is an upstream Konsole gap (KDE bug\n"
              << "  520743); bare URLs have it.\n\n";

    std::cout << bold << "Examples:" << rst << "\n"
              << "  progressive-cli login --homeserver https://matrix.org --username @me:matrix.org --password s3cret\n"
              << "  progressive-cli rooms\n"
              << "  progressive-cli view \"#general:matrix.org\" 50\n"
              << "  progressive-cli send \"#general:matrix.org\" \"Hello from CLI!\"\n"
              << "  progressive-cli llm \"explain this code\" --rich\n"
              << "  progressive-cli demo                        # the interactive demo session\n"
              << "  progressive-cli serve --port=29325\n"
              << "\n"
              << "The detailed flags: progressive-cli <command> --help\n"
              << dim << "The formatting is on only when attached to a terminal;"
              << rst << "\n"
              << dim << "--disable-formatting (or NO_COLOR=1) forces plain text."
              << rst << "\n";
}

std::string versionString() {
    return "progressive-cli v0.5.2";
}

void printVersion() {
    std::cout << versionString() << std::endl;
}

}} // namespace matrixcli::cli

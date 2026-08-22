#include <iostream>
#include <csignal>
#include <signal.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <set>
#include <map>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "config.hpp"
#include "media_send.hpp"
#include "main_commands.hpp"
#include "cli/args.hpp"
#include "commands.hpp"
#include "core/http_client.hpp"
#include "core/crypto/media_crypto.hpp"
#include <simdjson.h>
#include "globals.hpp"
#include "pcore.hpp"
#include "agent_tools.hpp"
#include "matrix_agent.hpp"
#include "ascii_ui.hpp"
#include "core/crash_handler.hpp"
#include "server/server.hpp"
#include "../lib/matrix/client.hpp"
#include "../lib/tdlib/tdlib_bridge.hpp"
#include "../lib/irc/irc_client.hpp"
#include "../lib/lemmy/lemmy_client.hpp"
#include "../lib/deltachat/dc_bridge.hpp"
#include "../lib/matrix/pushrules.hpp"
#include "../lib/database/db.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/util/notifications.hpp"
#include "../lib/util/string_utils.hpp"
#include "../lib/util/layout_remap.hpp"
#include "../lib/util/client_utils.hpp"

#ifdef BUILD_TUI
#include "../lib/tui/screen.hpp"
#include "../lib/tui/login.hpp"
#include "../lib/tui/agent_setup.hpp"
#include "../lib/tui/main_view.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"
#endif

// Global bridge instances (defined here, declared in globals.hpp)
namespace matrixcli {
    std::atomic<bool> g_interrupted{true};
    std::atomic<bool> g_agentInterrupt{false};
    tdlib::TdBridge g_tdlib;
    lemmy::LemmyClient g_lemmy;
    deltachat::DcBridge g_dc;
    std::map<std::string, std::vector<std::pair<std::string, int>>> g_msgQueue;
    std::mutex g_queueMutex;
    util::TypingMonitor g_typing;
    std::vector<std::string> g_notifyKeywords;
}

void signalHandler(int) {
    g_running = false;
    matrixcli::g_interrupted = false;
}

// The SAS verification core (defined in e2ee_commands.cpp) — shared by the
// CLI `verify` command and the TUI /verify slash.
extern int runSasVerification(const std::string& targetUser,
                              const std::string& targetDevice,
                              int timeoutSec, bool autoConfirm,
                              const std::function<void(const std::string&)>& log,
                              const std::function<bool()>& confirm);












// ---- Attach a file (media upload + send) ----
// matrixcli attach <room> <file> [--caption text]
// Plain rooms: upload + m.image/m.file/m.audio message. Encrypted rooms:
// the file is AES-CTR-encrypted client-side and sent as the m.encrypted
// "file" block (Element-compatible). Determines msgtype from the extension.
namespace matrixcli {

} // namespace matrixcli







// A proper-looking display name for the demo members (Alice, Bob, ...) —
// it differs from the mxid localpart, so the user list shows the
// highlighted "Displayname (mxid)" form.
// Populate the offline demo database (rooms + messages + reply chain).
// Shared by `demo populate` and the interactive `demo` REPL.
// populateDemoData, cmdDemoRepl and cmdTUI moved to demo_tui.cpp.
extern int populateDemoData(matrixcli::db::Database& dbi);
extern int cmdDemoRepl(const matrixcli::cli::Args& args);
#ifdef BUILD_TUI
extern int cmdTUI(const matrixcli::cli::Args& args);
#endif


int main(int argc, char* argv[]) {
    // sigaction WITHOUT SA_RESTART: the blocking stdin read must return
    // EINTR on Ctrl+C so the REPL loops can observe g_interrupted. The
    // plain signal() (glibc) restarts the read and Ctrl+C looks dead.
    {
        struct sigaction sa{};
        sa.sa_handler = signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
    }
    signal(SIGTERM, signalHandler);
    bool quietStart = argc >= 2 && std::string(argv[1]) == "about";
    progressive::crash::installCrashHandler(quietStart);

    // Register all commands via registry (extensible, no if/else)
    extern void registerBuiltinCommands();
    registerBuiltinCommands();

    auto args = matrixcli::cli::parseArgs(argc, argv);

    // Apply the persisted proxy (config.json "proxy_*") to the core's global
    // proxy BEFORE any command runs — every request then goes through it.
    extern void applyProxyFromConfig();
    applyProxyFromConfig();

    // Wrong keyboard layout: if the typed command is not recognized but its
    // layout-swapped form is a known command, reinterpret it. Opt-in via the
    // config key "fuzzy_layout" (on/true/1); off by default.
    try { matrixcli::Config::instance().load("config.json"); } catch (...) {}
    {
        std::string fl = matrixcli::Config::instance().get("fuzzy_layout", "off");
        if ((fl == "on" || fl == "true" || fl == "1") && !args.command.empty()) {
            static const char* kTop[] = {
                "help", "version", "serve", "login", "rooms", "spaces", "view",
                "status", "send", "attach", "send-file", "search", "config",
                "demo", "ui", "ascii", "tui", "td", "irc", "lemmy", "dc",
                "deltachat", nullptr};
            auto known = [&](const std::string& s) {
                if (matrixcli::CommandRegistry::instance().findCli(s))
                    return true;
                for (int i = 0; kTop[i]; ++i)
                    if (s == kTop[i]) return true;
                return false;
            };
            std::string r = matrixcli::util::keyboardLayoutRemap(args.command);
            if (r != args.command && known(r)) args.command = r;
        }
    }

    if (args.options.contains("version")) {
        matrixcli::cli::printVersion();
        return 0;
    }

    // Just "--mobile" (no command): launch the smartphone ASCII UI right
    // away — Element Classic style, Termux-friendly. Any extra flags are
    // forwarded (--static/--rows/--scroll) and an optional room id opens
    // the Chat tab.
    if (args.command.empty() && args.options.contains("mobile")) {
        matrixcli::cli::Args uiArgs;
        if (!args.positional.empty()) {
            uiArgs.positional.push_back(args.positional[0]);
        }
        uiArgs.options["mobile"] = "true";
        if (args.options.contains("static") || args.options.contains("once") ||
            args.options.contains("print")) {
            uiArgs.options["static"] = "true";
        }
        if (args.options.contains("rows")) uiArgs.options["rows"] = args.options.at("rows");
        if (args.options.contains("scroll")) uiArgs.options["scroll"] = args.options.at("scroll");
        return matrixcli::cmdAsciiUi(uiArgs);
    }

    if (args.command.empty() || args.command == "help" || args.options.contains("help")) {
        matrixcli::cli::printUsage(args);
        return 0;
    }

    if (args.command == "version") {
        matrixcli::cli::printVersion();
        return 0;
    }

    if (args.command == "serve") {
        return cmdServe(args);
    }

    if (args.command == "login") {
        return cmdLogin(args);
    }

    if (args.command == "rooms") {
        return cmdRooms(args);
    }

    if (args.command == "spaces") {
        return cmdSpaces(args);
    }

    if (args.command == "view") {
        return cmdView(args);
    }

    if (args.command == "status") {
        return cmdStatus(args);
    }

    if (args.command == "send") {
        return cmdSendMsg(args);
    }

    if (args.command == "attach" || args.command == "send-file") {
        return matrixcli::cmdAttachFile(args);
    }

    if (args.command == "search") {
        return cmdSearch(args);
    }

    if (args.command == "config") {
        return cmdConfig(args);
    }

    if (args.command == "demo") {
        if (!args.positional.empty() && args.positional[0] == "populate") {
            return cmdDemoPopulate(args);
        }
        return cmdDemoRepl(args);
    }

    if (args.command == "ui" || args.command == "ascii") {
        return matrixcli::cmdAsciiUi(args);
    }

#ifdef BUILD_TUI
    if (args.command == "tui") {
        return cmdTUI(args);
    }
#endif

    // Default: launch TUI if available
#ifdef BUILD_TUI
    if (args.command.empty()) {
        return cmdTUI(args);
    }
#endif

    if (args.command == "td") return cmdTdBridge(args);
    if (args.command == "irc") return cmdIrcBridge(args);
    if (args.command == "lemmy") return cmdLemmyBridge(args);
    if (args.command == "dc" || args.command == "deltachat") return cmdDcBridge(args);

    // Try command registry (extensible, no if/else needed)
    auto cliHandler = matrixcli::CommandRegistry::instance().findCli(args.command);
    if (cliHandler) return cliHandler(args);

    std::cerr << "Unknown command: " << args.command << "\n"
              << "Run 'progressive-cli --help' for usage." << std::endl;
    return 1;
}


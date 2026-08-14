#pragma once

#include "../lib/tdlib/tdlib_bridge.hpp"
#include "../lib/lemmy/lemmy_client.hpp"
#include "../lib/deltachat/dc_bridge.hpp"
#include "../lib/util/client_utils.hpp"

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

namespace matrixcli {

// Bridge instances (shared across CLI and TUI)
extern tdlib::TdBridge g_tdlib;
extern lemmy::LemmyClient g_lemmy;
extern deltachat::DcBridge g_dc;
extern std::map<std::string, std::vector<std::pair<std::string, int>>> g_msgQueue;
extern std::mutex g_queueMutex;
extern util::TypingMonitor g_typing;
extern std::vector<std::string> g_notifyKeywords;

// Set false by the SIGINT/SIGTERM handler; long-running commands (verify,
// serve loops) must poll it and bail out promptly so Ctrl+C always works.
extern std::atomic<bool> g_interrupted;
// Esc during an agent run: stop the agent, keep the program alive.
extern std::atomic<bool> g_agentInterrupt;

} // namespace matrixcli

#pragma once

#include "../lib/tdlib/tdlib_bridge.hpp"
#include "../lib/lemmy/lemmy_client.hpp"
#include "../lib/deltachat/dc_bridge.hpp"
#include "../lib/util/client_utils.hpp"

#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace matrixcli {

// Bridge instances (shared across CLI and TUI)
extern tdlib::TdBridge g_tdlib;
extern lemmy::LemmyClient g_lemmy;
extern deltachat::DcBridge g_dc;
extern std::map<std::string, std::vector<std::pair<std::string, int>>> g_msgQueue;
extern std::mutex g_queueMutex;
extern util::TypingMonitor g_typing;
extern std::vector<std::string> g_notifyKeywords;

} // namespace matrixcli

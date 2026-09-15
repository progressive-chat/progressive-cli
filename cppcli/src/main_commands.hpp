// main_commands.hpp — the CLI handlers split out of main.cpp.
#pragma once

#include <string>
#include <utility>

#include "cli/args.hpp"

int cmdServe(const matrixcli::cli::Args& args);
int cmdLogin(const matrixcli::cli::Args& args);
int cmdStatus(const matrixcli::cli::Args& args);
int cmdRooms(const matrixcli::cli::Args& args);
int cmdSpaces(const matrixcli::cli::Args& args);
int cmdView(const matrixcli::cli::Args& args);
int cmdSendMsg(const matrixcli::cli::Args& args);
int cmdSearch(const matrixcli::cli::Args& args);
int cmdConfig(const matrixcli::cli::Args& args);
int cmdDemoPopulate(const matrixcli::cli::Args& args);

int cmdTdBridge(const matrixcli::cli::Args& args);
int cmdIrcBridge(const matrixcli::cli::Args& args);
int cmdLemmyBridge(const matrixcli::cli::Args& args);
int cmdDcBridge(const matrixcli::cli::Args& args);

// Telegram API credentials for TDLib: the in-source values are only the
// public test defaults — config.json "tdlib_api_id" / "tdlib_api_hash"
// (your own credentials from https://my.telegram.org) win when set.
namespace matrixcli {
std::pair<int, std::string> tdlibApiCredentials();
}

namespace matrixcli {
int cmdAttachFile(const cli::Args& args);
int cmdTtys(const cli::Args& args);
}

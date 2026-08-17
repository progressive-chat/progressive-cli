// main_commands.hpp — the CLI handlers split out of main.cpp.
#pragma once

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

namespace matrixcli {
int cmdAttachFile(const cli::Args& args);
}

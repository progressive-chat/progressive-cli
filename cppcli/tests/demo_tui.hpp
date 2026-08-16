// demo_tui.hpp — the demo/TUI handlers split out of demo_tui.cpp.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"
#include "cli/args.hpp"
#include "../lib/tui/screen.hpp"
#include "../lib/tui/chat_view.hpp"
#include "../lib/tui/config.hpp"

int populateDemoData(matrixcli::db::Database& dbi);
void populateDemoDataExtras(matrixcli::db::Database& dbi);
int cmdDemoRepl(const matrixcli::cli::Args& args);
int cmdTUI(const matrixcli::cli::Args& args);

// The URL-preview queue helper (shared by the TUI command + event
// handlers). Defined in cmd_tui_commands.cpp.
void queueUrlPreview(matrixcli::matrix::Client& client,
                     matrixcli::tui::ChatView& chat,
                     const std::string& roomId, const std::string& text);

void tuiHandleCommand(matrixcli::tui::ChatView& chat,
                      matrixcli::tui::Screen& screen,
                      matrixcli::matrix::Client& client,
                      matrixcli::db::Database& dbi,
                      const matrixcli::tui::TUIConfig& tuiCfg,
                      std::atomic<bool>& sasConfirm,
                      std::atomic<bool>& sasCancel,
                      const std::function<void(const std::string&)>& launchAgent,
                      const std::string& cmd, const std::string& argsText);

void tuiHandleEvent(matrixcli::tui::ChatView& chat,
                    matrixcli::tui::Screen& screen,
                    matrixcli::matrix::Client& client,
                    matrixcli::db::Database& dbi,
                    const matrixcli::tui::TUIConfig& tuiCfg,
                    const matrix::Event& ev);

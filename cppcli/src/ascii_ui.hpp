// src/ascii_ui.hpp — ASCII-drawn client interface (not the TUI).
#pragma once
#include "cli/args.hpp"

namespace matrixcli {
namespace db { class Database; }
int cmdAsciiUi(const cli::Args& args);
int cmdAbout(const cli::Args& args);

// Mini line editor with the command history: Up/Down arrows navigate the
// history, Backspace edits, Enter submits, Ctrl+C returns false. Falls
// back to a plain getline when stdin is not a terminal.
bool readLineWithHistory(std::vector<std::string>& history,
                         const std::string& prompt, std::string& out);
void printAbout(const std::string& proxyLabel, const std::string& accountLabel);
// ANSI markdown renderer (bold/code/fences/links/lists) — shared with
// the llm CLI output (the wrapper over the internal renderer).
std::string renderMarkdownAnsi(const std::string& body);
// Real media-upload send (defined in main.cpp).
int cmdAttachFile(const cli::Args& args);
// Offline (no session) file message: inserts an m.file event locally.
int uiInsertLocalFile(db::Database& dbi, const std::string& roomId,
                      const std::string& path, const std::string& caption,
                      const std::string& threadRoot = "");
} // namespace matrixcli

void registerAsciiUiCommand();

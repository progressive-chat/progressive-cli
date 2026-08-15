#pragma once

// The LLM conversation store: ~/.local/share/matrixcli/sessions (XDG).
// The logic lives here (not in the CLI file) so the tests can pin the
// paths, the sanitization and the archive behavior.

#include <string>

namespace matrixcli { namespace util {

// The sessions directory (~/.local/share/matrixcli/sessions, or
// $XDG_DATA_HOME/matrixcli/sessions).
std::string llmSessionsDir();

// The session file for the name: llm-chat.json for "", llm-<sanitized>.json
// otherwise. The name keeps [a-zA-Z0-9._-], anything else becomes '_'.
std::string llmSessionPathFor(const std::string& name);

// The --fresh start: the existing session is renamed with a timestamp
// (never destroyed). No-op when there is nothing to archive.
void archiveSession(const std::string& name);

}} // namespace matrixcli::util

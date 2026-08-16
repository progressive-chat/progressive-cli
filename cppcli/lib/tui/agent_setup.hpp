#pragma once

#include <string>

namespace matrixcli { namespace tui {

class Screen;

struct AgentSetupResult {
    bool ok = false;       // the setup completed (Enter on the last field)
    bool cancelled = false; // Esc / Ctrl+C on a prompt
    std::string provider;  // the preset name, or "custom"
    std::string endpoint;
    std::string model;
    std::string key;
    std::string proxy;     // SOCKS5 "host:port", empty = direct
};

// The first-run agent API setup wizard. Shown when the TUI agent mode
// (`progressive-cli tui agent`) starts without a configured API key: pick a
// provider preset (opencode-style list), enter the key and optionally
// override the model/endpoint/proxy. The caller persists the result into
// ~/.config/matrixcli/agent.json.
class AgentSetupView {
public:
    AgentSetupResult run(Screen& screen);
};

}} // namespace matrixcli::tui

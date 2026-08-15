#include "agent_setup.hpp"
#include "screen.hpp"
#include "llm_presets.hpp"

#ifdef HAS_NCURSES
#include <ncurses.h>
#endif
#include <cctype>

namespace matrixcli { namespace tui {

namespace {

// Read one line at the given row. `label` is centered above the input.
// Returns false when the read was interrupted (Esc/Ctrl+C).
bool readLine(Screen& screen, int y, const std::string& label,
              std::string& out, bool masked) {
#ifdef HAS_NCURSES
    screen.drawTextCentered(y, label);
    if (masked) noecho(); else echo();
    move(y + 1, 2);
    char buf[512] = {};
    int rc = wgetnstr(stdscr, buf, 511);
    if (masked) noecho(); else echo();
    if (rc == ERR) return false;
    out = buf;
    return true;
#else
    (void)screen; (void)y; (void)label; (void)out; (void)masked;
    return false;
#endif
}

} // namespace

AgentSetupResult AgentSetupView::run(Screen& screen) {
    AgentSetupResult result;

#ifdef HAS_NCURSES
    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);

    // 1. The provider preset menu.
    clear();
    screen.drawTextCentered(1, "Agent API setup (first run)");
    screen.drawTextCentered(2, "Choose the LLM provider:");
    int y = 4;
    const auto& presets = util::llmPresets();
    for (size_t i = 0; i < presets.size(); i++) {
        const auto& p = presets[i];
        std::string line = " " + std::to_string(i + 1) + ") " + p.name;
        while (line.size() < 16) line += ' ';
        line += p.model;
        if (p.local) line += "  (local, no key)";
        screen.drawText(2, y++, line);
    }
    screen.drawText(2, y++, " 0) custom endpoint");
    screen.drawText(2, y, "Enter the number (Esc+Enter = cancel):");

    std::string choice;
    if (!readLine(screen, y, "", choice, false) || choice.empty()) {
        result.cancelled = true;
        goto done;
    }
    {
        int n = -1;
        try { n = std::stoi(choice); } catch (...) { n = -1; }
        if (n > 0 && n <= static_cast<int>(presets.size())) {
            const auto& p = presets[n - 1];
            result.provider = p.name;
            result.endpoint = p.endpoint;
            result.model = p.model;
        } else if (n == 0) {
            result.provider = "custom";
        } else {
            result.cancelled = true;
            goto done;
        }
    }

    // 2. The endpoint (custom, or override the preset default).
    clear();
    {
        std::string label = "API base URL (Enter = " + result.endpoint + "):";
        std::string value;
        if (!readLine(screen, 4, label, value, false)) { result.cancelled = true; goto done; }
        if (!value.empty()) result.endpoint = value;
        if (result.endpoint.empty()) result.endpoint = "https://api.openai.com";
    }

    // 3. The model (override the preset default).
    clear();
    {
        std::string label = "Model (Enter = " + result.model + "):";
        std::string value;
        if (!readLine(screen, 4, label, value, false)) { result.cancelled = true; goto done; }
        if (!value.empty()) result.model = value;
    }

    // 4. The API key (masked; optional for the local servers).
    clear();
    {
        const auto* preset = util::findLlmPreset(result.provider);
        std::string label = preset && preset->local
                                ? "API key (Enter = none for " + result.provider + "):"
                                : "API key (hidden):";
        std::string value;
        if (!readLine(screen, 4, label, value, true)) { result.cancelled = true; goto done; }
        result.key = value;
    }

    // 5. The proxy (optional, Tor/I2P).
    clear();
    {
        std::string label = "SOCKS5 proxy host:port (Enter = direct):";
        std::string value;
        if (!readLine(screen, 4, label, value, false)) { result.cancelled = true; goto done; }
        result.proxy = value;
    }

    result.ok = true;

done:
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    clear();
#endif

    return result;
}

}} // namespace matrixcli::tui

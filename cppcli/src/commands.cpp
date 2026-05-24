#include "commands.hpp"

namespace matrixcli {

CommandRegistry& CommandRegistry::instance() {
    static CommandRegistry reg;
    return reg;
}

void CommandRegistry::registerCli(const std::string& name, CliHandler handler, const std::string& help) {
    _cli[name] = std::move(handler);
    if (!help.empty()) _cliHelp[name] = help;
}

void CommandRegistry::registerTui(const std::string& name, TuiHandler handler, const std::string&) {
    _tui[name] = std::move(handler);
}

CliHandler CommandRegistry::findCli(const std::string& name) const {
    auto it = _cli.find(name);
    return it != _cli.end() ? it->second : nullptr;
}

TuiHandler CommandRegistry::findTui(const std::string& name) const {
    auto it = _tui.find(name);
    return it != _tui.end() ? it->second : nullptr;
}

std::vector<std::string> CommandRegistry::cliCommands() const {
    std::vector<std::string> result;
    for (auto& [k, _] : _cli) result.push_back(k);
    return result;
}

std::vector<std::string> CommandRegistry::tuiCommands() const {
    std::vector<std::string> result;
    for (auto& [k, _] : _tui) result.push_back(k);
    return result;
}

std::string CommandRegistry::cliHelp(const std::string& name) const {
    auto it = _cliHelp.find(name);
    return it != _cliHelp.end() ? it->second : "";
}

} // namespace matrixcli

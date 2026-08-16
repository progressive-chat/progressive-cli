#pragma once

#include "cli/args.hpp"
#include <string>
#include <functional>
#include <map>
#include <memory>

namespace matrixcli {

// CLI command handler: returns exit code
using CliHandler = std::function<int(const cli::Args&)>;

// TUI command handler: called with args string
using TuiHandler = std::function<void(const std::string& args)>;

class CommandRegistry {
public:
    static CommandRegistry& instance();

    void registerCli(const std::string& name, CliHandler handler, const std::string& help = "");
    void registerTui(const std::string& name, TuiHandler handler, const std::string& help = "");

    CliHandler findCli(const std::string& name) const;
    TuiHandler findTui(const std::string& name) const;

    std::vector<std::string> cliCommands() const;
    std::vector<std::string> tuiCommands() const;
    std::string cliHelp(const std::string& name) const;

private:
    CommandRegistry() = default;
    std::map<std::string, CliHandler> _cli;
    std::map<std::string, TuiHandler> _tui;
    std::map<std::string, std::string> _cliHelp;
};

} // namespace matrixcli

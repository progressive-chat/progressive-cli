#pragma once

#include <string>
#include <map>
#include <vector>
#include <optional>

namespace matrixcli { namespace cli {

struct Args {
    std::string command;
    std::map<std::string, std::string> options;
    std::vector<std::string> positional;
};

Args parseArgs(int argc, char* argv[]);

// The help output is coloured/formatted only when stdout is a terminal,
// unless --disable-formatting (or NO_COLOR=1, TERM=dumb) forces plain text.
void printUsage(const Args& args);
std::string versionString();  // "progressive-cli vX.Y.Z"
void printVersion();

}} // namespace matrixcli::cli

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

void printUsage();
void printVersion();

}} // namespace matrixcli::cli

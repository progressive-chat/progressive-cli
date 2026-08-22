#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <tuple>
#include "cli/args.hpp"
#include "../lib/database/db.hpp"
#include "../lib/matrix/client.hpp"

// Demo showcase helpers + offline demo flows, split out of demo_repl.cpp so
// that file stays under the 1000-line guideline. Declared here and defined in
// demo_showcases.cpp; demo_repl.cpp #includes this header.

std::string demoShortSender(const std::string& s);
std::string demoFormatTs(int64_t ts);
std::string demoRoomDisplayName(matrixcli::db::Database& dbi,
                                const std::string& roomId);
std::string demoPickRoom(matrixcli::db::Database& dbi, bool sortByMembers,
                         bool allowSearch);
matrixcli::matrix::Event demoPickMessage(matrixcli::db::Database& dbi,
                                          const std::string& roomId);
std::vector<std::tuple<std::string, std::string, std::string>>
demoRoomMembers(const std::string& roomId, int count);

int demoMembersShowcase(matrixcli::db::Database& dbi,
                        const std::string& roomArg);
int demoTypingShowcase(matrixcli::db::Database& dbi);
int demoEditShowcase(matrixcli::db::Database& dbi);
int demoReportShowcase(matrixcli::db::Database& dbi);
int demoTopicShowcase(matrixcli::db::Database& dbi);
int demoThreadsShowcase(matrixcli::db::Database& dbi);
int demoConfigShowcase(matrixcli::db::Database& dbi);
int demoBackupShowcase(matrixcli::db::Database& dbi);

void demoMarkdownShowcase();
void demoVoteShowcase(matrixcli::db::Database& dbi);

// Whether to reinterpret input typed with the wrong keyboard layout (config
// key "fuzzy_layout" = on/true/1). Off by default.
bool demoFuzzyLayout();

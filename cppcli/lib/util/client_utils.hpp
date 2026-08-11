#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace matrixcli { namespace util {

// ── Display name utils (from displayname_utils.cpp) ──

// Extract username from MXID: @alice:matrix.org → alice
std::string userIdToName(const std::string& mxid);

// Get first letter for avatar fallback (handles @/#/+ prefixes, emoji)
std::string avatarLetter(const std::string& name);

// Generate consistent color from user ID (DJB2 hash → hue)
int userIdToHue(const std::string& mxid);

// Format member name with power level badge: ★Alice, ☆Bob
std::string formatMemberName(const std::string& name, int power_level);

// ── Emoji analyzer (from emoji_analyzer.cpp) ──

struct EmojiStats {
    std::map<std::string, int> counts;  // emoji → count
    int total = 0;
    double emoji_ratio = 0; // messages with emoji / total
};

// Check if text contains only emoji
bool isEmojiOnly(const std::string& text);

// Count emoji occurrences in text
int countEmoji(const std::string& text);

// Get top N most used emojis
std::vector<std::pair<std::string, int>> topEmoji(const EmojiStats& stats, int n = 10);

// ── Typing monitor (from typing_monitor.cpp) ──

class TypingMonitor {
public:
    void updateUser(const std::string& room_id, const std::string& user_id, int64_t ts_ms);
    void pruneExpired(int64_t now_ms, int timeout_ms = 30000);
    std::vector<std::string> activeUsers(const std::string& room_id) const;
    std::string formatTypingUsers(const std::string& room_id) const;

private:
    std::map<std::string, std::map<std::string, int64_t>> _typing; // room → user → timestamp
};

}} // namespace matrixcli::util

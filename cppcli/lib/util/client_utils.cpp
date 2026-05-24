#include "client_utils.hpp"
#include <algorithm>
#include <map>

namespace matrixcli { namespace util {

// ── Display name ──

std::string userIdToName(const std::string& mxid) {
    if (mxid.empty() || mxid[0] != '@') return mxid;
    auto pos = mxid.find(':');
    if (pos == std::string::npos) return mxid.substr(1);
    return mxid.substr(1, pos - 1);
}

std::string avatarLetter(const std::string& name) {
    if (name.empty()) return "?";
    // Handle emoji (surrogate pairs)
    unsigned char c = name[0];
    if (c >= 0xF0) return name.substr(0, 4); // 4-byte UTF-8
    if (c >= 0xE0) return name.substr(0, 3);
    if (c >= 0xC0) return name.substr(0, 2);
    if (name[0] == '@' || name[0] == '#' || name[0] == '!') {
        return name.size() > 1 ? std::string(1, name[1]) : "?";
    }
    return std::string(1, (char)std::toupper(name[0]));
}

int userIdToHue(const std::string& mxid) {
    // DJB2 hash
    unsigned long hash = 5381;
    for (char c : mxid) hash = ((hash << 5) + hash) + c;
    return hash % 360;
}

std::string formatMemberName(const std::string& name, int power_level) {
    if (power_level >= 100) return "★" + name;
    if (power_level >= 50) return "☆" + name;
    return "  " + name;
}

// ── Emoji ──

static bool isEmojiChar(uint32_t cp) {
    return (cp >= 0x1F600 && cp <= 0x1F64F) || // Emoticons
           (cp >= 0x1F300 && cp <= 0x1F5FF) || // Misc Symbols
           (cp >= 0x1F680 && cp <= 0x1F6FF) || // Transport
           (cp >= 0x2600 && cp <= 0x26FF)   || // Misc
           (cp >= 0x2700 && cp <= 0x27BF)   || // Dingbats
           (cp >= 0xFE00 && cp <= 0xFE0F)   || // Variation Selectors
           (cp >= 0x1F900 && cp <= 0x1F9FF) || // Supplemental
           (cp >= 0x1FA00 && cp <= 0x1FA6F) || // Chess
           (cp >= 0x1FA70 && cp <= 0x1FAFF);   // Symbols Extended-A
}

bool isEmojiOnly(const std::string& text) {
    if (text.empty()) return false;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = (unsigned char)text[i];
        if (cp < 0x80) { if (cp != ' ' && cp != 0xFE0F) return false; i++; continue; }
        // Basic multibyte detection
        if ((cp & 0xE0) == 0xC0) { cp = ((cp & 0x1F) << 6) | (text[i+1] & 0x3F); i+=2; }
        else if ((cp & 0xF0) == 0xE0) { cp = ((cp & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F); i+=3; }
        else if ((cp & 0xF8) == 0xF0) { cp = ((cp & 0x07) << 18) | ((text[i+1] & 0x3F) << 12) | ((text[i+2] & 0x3F) << 6) | (text[i+3] & 0x3F); i+=4; }
        else { i++; continue; }
        if (!isEmojiChar(cp)) return false;
    }
    return true;
}

int countEmoji(const std::string& text) {
    int count = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = text[i];
        if (c < 0x80) { i++; continue; }
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (text[i+1] & 0x3F); i+=2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F); i+=3; }
        else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | ((text[i+1] & 0x3F) << 12) | ((text[i+2] & 0x3F) << 6) | (text[i+3] & 0x3F); i+=4; }
        else { i++; continue; }
        if (isEmojiChar(cp)) count++;
    }
    return count;
}

std::vector<std::pair<std::string, int>> topEmoji(const EmojiStats& stats, int n) {
    std::vector<std::pair<std::string, int>> sorted(stats.counts.begin(), stats.counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    if ((int)sorted.size() > n) sorted.resize(n);
    return sorted;
}

// ── Typing monitor ──

void TypingMonitor::updateUser(const std::string& room_id, const std::string& user_id, int64_t ts_ms) {
    _typing[room_id][user_id] = ts_ms;
}

void TypingMonitor::pruneExpired(int64_t now_ms, int timeout_ms) {
    for (auto& [room, users] : _typing) {
        for (auto it = users.begin(); it != users.end();) {
            if (now_ms - it->second > timeout_ms) it = users.erase(it);
            else ++it;
        }
    }
}

std::vector<std::string> TypingMonitor::activeUsers(const std::string& room_id) const {
    std::vector<std::string> result;
    auto it = _typing.find(room_id);
    if (it != _typing.end()) {
        for (auto& [user, _] : it->second) result.push_back(user);
    }
    return result;
}

std::string TypingMonitor::formatTypingUsers(const std::string& room_id) const {
    auto users = activeUsers(room_id);
    if (users.empty()) return "";
    if (users.size() == 1) return userIdToName(users[0]) + " is typing...";
    if (users.size() <= 3) {
        std::string s;
        for (size_t i = 0; i < users.size(); i++) {
            if (i > 0) s += (i == users.size() - 1 ? " and " : ", ");
            s += userIdToName(users[i]);
        }
        return s + " are typing...";
    }
    return userIdToName(users[0]) + " and " + std::to_string(users.size() - 1) + " others are typing...";
}

}} // namespace matrixcli::util

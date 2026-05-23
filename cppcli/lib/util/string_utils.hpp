#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <functional>

namespace matrixcli { namespace util {

inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delimiter)) {
        tokens.push_back(trim(token));
    }
    return tokens;
}

inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

inline std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (auto c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%'
                    << std::uppercase << std::setw(2)
                    << static_cast<int>(static_cast<unsigned char>(c))
                    << std::nouppercase;
        }
    }
    return escaped.str();
}

}} // namespace matrixcli::util

// ── ANSI colors ──
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_DIM     "\033[2m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GRAY    "\033[90m"

inline std::string ansiUser(const std::string& user_id, const std::string& text) {
    int colors[] = {32,34,35,36,33,31,32}; // green blue magenta cyan yellow red green
    size_t h = std::hash<std::string>{}(user_id);
    int color = colors[h % 7];
    return "\033[1;" + std::to_string(color) + "m" + text + ANSI_RESET;
}

// Relative time formatting

// Relative time formatting
inline std::string relativeTime(int64_t ts_ms) {
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t diff = now - ts_ms / 1000;
    if (diff < 60) return "now";
    if (diff < 3600) return std::to_string(diff / 60) + "m ago";
    if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
    if (diff < 604800) return std::to_string(diff / 86400) + "d ago";
    time_t t = ts_ms / 1000;
    char buf[12];
    strftime(buf, sizeof(buf), "%d %b", localtime(&t));
    return buf;
}

// Day separator: "── Today ──", "── Yesterday ──", "── Mon 15 May ──"
inline std::string daySeparator(int64_t ts_ms) {
    time_t now_t = time(nullptr);
    time_t msg_t = ts_ms / 1000;

    struct tm now_tm, msg_tm;
    localtime_r(&now_t, &now_tm);
    localtime_r(&msg_t, &msg_tm);

    // Same day?
    if (now_tm.tm_year == msg_tm.tm_year && now_tm.tm_yday == msg_tm.tm_yday)
        return "── Today ──";
    // Yesterday?
    if (now_tm.tm_year == msg_tm.tm_year && now_tm.tm_yday == msg_tm.tm_yday + 1)
        return "── Yesterday ──";

    char buf[32];
    strftime(buf, sizeof(buf), "── %a %d %b %Y ──", &msg_tm);
    return buf;
}

// User color from user_id
inline int userColor(const std::string& user_id) {
    size_t h = std::hash<std::string>{}(user_id);
    // 7 nice terminal colors
    int colors[] = {1,2,3,4,5,6,2};
    return colors[h % 7];
}

// Link detection
inline std::string extractLink(const std::string& text) {
    auto pos = text.find("https://");
    if (pos == std::string::npos) pos = text.find("http://");
    if (pos == std::string::npos) return "";
    auto end = text.find_first_of(" \n\r\t\"'<", pos);
    if (end == std::string::npos) end = text.size();
    return text.substr(pos, end - pos);
}

// Reply context helper
inline std::string replyContext(const std::string& reply_to_body) {
    if (reply_to_body.size() > 60) return "↩ \"" + reply_to_body.substr(0, 57) + "...\"";
    return "↩ \"" + reply_to_body + "\"";
}

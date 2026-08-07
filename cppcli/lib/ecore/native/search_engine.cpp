#include "progressive/search_engine.hpp"
#include "progressive/eventdb.hpp"
#include "progressive/string_utils.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace progressive {

static bool caseInsensitiveContains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                       std::tolower(static_cast<unsigned char>(b)); });
    return it != haystack.end();
}

std::string RoomSearchEngine::search(const std::string& roomId, const std::string& term,
                                  int limit, int offset) const {
    if (!db_) return "[]";

    std::vector<DbEvent> events = db_->getEvents(roomId, limit + offset, 0);
    std::ostringstream out;
    out << "[";
    bool first = true;
    int skipped = 0, added = 0;

    for (auto& e : events) {
        if (caseInsensitiveContains(e.body, term) ||
            caseInsensitiveContains(e.senderName, term)) {
            if (skipped < offset) { skipped++; continue; }
            if (added >= limit) break;
            if (!first) out << ",";
            first = false;
            out << "{"
                << "\"eventId\":\"" << escapeJson(e.eventId) << "\","
                << "\"senderId\":\"" << escapeJson(e.senderId) << "\","
                << "\"senderName\":\"" << escapeJson(e.senderName) << "\","
                << "\"body\":\"" << escapeJson(e.body) << "\","
                << "\"timestamp\":\"" << escapeJson(e.timestamp) << "\","
                << "\"originServerTs\":" << e.originServerTs << ","
                << "\"isEncrypted\":" << (e.isEncrypted ? "true" : "false")
                << "}";
            added++;
        }
    }
    out << "]";
    return out.str();
}

} // namespace progressive

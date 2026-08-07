#include "progressive/typing_utils.hpp"
#include <sstream>
#include <chrono>

namespace progressive {

TypingStatus parseTypingEvent(const std::string& json) {
    TypingStatus s;
    auto usersPos = json.find("\"user_ids\"");
    if (usersPos == std::string::npos) return s;
    
    auto arrStart = json.find('[', usersPos);
    auto arrEnd = json.find(']', arrStart);
    if (arrStart == std::string::npos || arrEnd == std::string::npos) return s;
    
    std::string arr = json.substr(arrStart + 1, arrEnd - arrStart - 1);
    size_t pos = 0;
    while (pos < arr.size()) {
        auto q1 = arr.find('"', pos);
        if (q1 == std::string::npos) break;
        auto q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        s.userIds.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
    }
    
    s.typingCount = (int)s.userIds.size();
    s.isAnyoneTyping = s.typingCount > 0;
    auto tsPos = json.find("\"origin_server_ts\":");
    if (tsPos != std::string::npos) {
        tsPos += 18;
        try { s.lastTypingEventMs = std::stoll(json.substr(tsPos)); } catch(...) {}
    }
    return s;
}

std::string buildTypingEvent(const std::vector<std::string>& userIds, bool typing, int timeoutMs) {
    std::ostringstream os;
    os << "{";
    os << R"("typing":)" << (typing ? "true" : "false");
    if (typing) {
        os << R"(,"user_ids":[)";
        for (size_t i = 0; i < userIds.size(); i++) {
            if (i > 0) os << ",";
            os << R"(")" << userIds[i] << R"(")";
        }
        os << "]";
    }
    os << R"(,"timeout":)" << timeoutMs;
    os << "}";
    return os.str();
}

std::string formatTypingText(const std::vector<TypingUserInfo>& typers, int maxNames) {
    if (typers.empty()) return "";
    if (typers.size() == 1) {
        std::string name = typers[0].displayName.empty() ? typers[0].userId : typers[0].displayName;
        return name + " is typing...";
    }
    if (typers.size() == 2) {
        auto n1 = typers[0].displayName.empty() ? typers[0].userId : typers[0].displayName;
        auto n2 = typers[1].displayName.empty() ? typers[1].userId : typers[1].displayName;
        return n1 + " and " + n2 + " are typing...";
    }
    std::ostringstream os;
    for (int i = 0; i < maxNames && i < (int)typers.size(); i++) {
        if (i > 0) os << ", ";
        os << (typers[i].displayName.empty() ? typers[i].userId : typers[i].displayName);
    }
    int remaining = (int)typers.size() - maxNames;
    if (remaining > 0) os << " and " << remaining << " other" << (remaining > 1 ? "s" : "");
    os << " are typing...";
    return os.str();
}

bool isTypingExpired(int64_t lastTypedMs, int64_t nowMs, int timeoutMs) {
    if (nowMs <= 0) {
        nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    return (nowMs - lastTypedMs) > timeoutMs;
}

} // namespace progressive

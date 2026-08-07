#include "progressive/typing_monitor.hpp"
#include "progressive/string_utils.hpp"
#include <sstream>
#include <algorithm>

namespace progressive {

TypingState updateTypingState(
    const std::string& roomId,
    const std::vector<std::string>& currentTypingUserIds,
    const std::vector<std::string>& displayNames,
    int64_t nowMs
) {
    TypingState state;
    state.roomId = roomId;
    state.snapshotTs = nowMs;

    for (size_t i = 0; i < currentTypingUserIds.size(); ++i) {
        TypingUser user;
        user.userId = currentTypingUserIds[i];
        user.displayName = (i < displayNames.size()) ? displayNames[i] : currentTypingUserIds[i];
        user.lastTypingTs = nowMs;
        user.isTyping = true;
        state.typingUsers.push_back(user);
    }

    state.typingCount = static_cast<int>(state.typingUsers.size());
    state.hasAnyoneTyping = !state.typingUsers.empty();
    return state;
}

bool isUserTyping(const TypingState& state, const std::string& userId, int64_t nowMs) {
    for (const auto& user : state.typingUsers) {
        if (user.userId == userId && user.isTyping) {
            return (nowMs - user.lastTypingTs) < TYPING_TIMEOUT_MS;
        }
    }
    return false;
}

TypingState pruneTypingUsers(const TypingState& state, int64_t nowMs) {
    TypingState pruned = state;
    pruned.typingUsers.clear();

    for (const auto& user : state.typingUsers) {
        if ((nowMs - user.lastTypingTs) < TYPING_TIMEOUT_MS) {
            pruned.typingUsers.push_back(user);
        }
    }

    pruned.typingCount = static_cast<int>(pruned.typingUsers.size());
    pruned.hasAnyoneTyping = !pruned.typingUsers.empty();
    pruned.snapshotTs = nowMs;
    return pruned;
}

std::string formatTypingText(const TypingState& state, int maxNames) {
    // Original Kotlin (TypingHelper.kt):
    //   fun getTypingText(context: Context, users: List<TypingUser>, max: Int): String
    //   Displays up to maxNames users, then "... and N others"
    int total = state.typingCount;

    if (total == 0) return "";
    if (total == 1) {
        return state.typingUsers[0].displayName + " is typing…";
    }

    std::ostringstream out;
    int shown = 0;
    for (int i = 0; i < total && shown < maxNames; ++i) {
        const auto& user = state.typingUsers[i];
        if (!user.isTyping) continue;
        if (shown > 0) {
            out << ", ";
            if (shown == maxNames - 1 && (total - shown) > 1) {
                out << "and " << (total - shown) << " others";
                break;
            }
            if (shown == total - 1) out << "and ";
        }
        out << user.displayName;
        shown++;
    }

    out << " are typing…";
    return out.str();
}

std::string typingStateToJson(const TypingState& state) {
    auto esc = [](const std::string& s) -> std::string {
        return escapeJson(s);
    };
    std::ostringstream json;
    json << R"({"roomId": ")" << esc(state.roomId) << R"(",)";
    json << R"("typingCount": )" << state.typingCount << ",";
    json << R"("hasAnyoneTyping": )" << (state.hasAnyoneTyping ? "true" : "false") << ",";
    json << R"("snapshotTs": )" << state.snapshotTs << ",";
    json << R"("typingUsers": [)";
    for (size_t i = 0; i < state.typingUsers.size(); ++i) {
        if (i > 0) json << ",";
        json << R"({"userId": ")" << esc(state.typingUsers[i].userId) << R"(",)";
        json << R"("displayName": ")" << esc(state.typingUsers[i].displayName) << R"(",)";
        json << R"("isTyping": )" << (state.typingUsers[i].isTyping ? "true" : "false");
        json << "}";
    }
    json << "]}";
    return json.str();
}

} // namespace progressive

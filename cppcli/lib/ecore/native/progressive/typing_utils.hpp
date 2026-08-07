#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace progressive {

struct TypingUserInfo {
    std::string userId;
    std::string displayName;
    int64_t lastTypedMs = 0;
};

struct TypingStatus {
    std::vector<std::string> userIds;  // users currently typing
    bool isAnyoneTyping = false;
    int typingCount = 0;
    int64_t lastTypingEventMs = 0;
};

// Parse m.typing event content
TypingStatus parseTypingEvent(const std::string& eventJson);

// Build m.typing event content
std::string buildTypingEvent(const std::vector<std::string>& userIds, bool typing, int timeoutMs = 30000);

// Format typing indicator text for display
// "Alice is typing..."
// "Alice and Bob are typing..."
// "Alice, Bob and 3 others are typing..."
std::string formatTypingText(const std::vector<TypingUserInfo>& typers, int maxNames = 3);

// Check if a typing timeout has expired
bool isTypingExpired(int64_t lastTypedMs, int64_t nowMs = 0, int timeoutMs = 30000);

} // namespace progressive

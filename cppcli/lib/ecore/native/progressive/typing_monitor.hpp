#ifndef PROGRESSIVE_TYPING_MONITOR_HPP
#define PROGRESSIVE_TYPING_MONITOR_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace progressive {

// ---- Typing Indicator Monitor ----
// Ported from: im.vector.app.features.home.room.detail.compose.TypingView.kt
//              org.matrix.android.sdk.api.session.room.typing.TypingUsersTracker.kt
//              TypingHelper.kt (Element SDK)

struct TypingUser {
    std::string userId;
    std::string displayName;
    int64_t lastTypingTs = 0;   // epoch ms
    bool isTyping = false;
};

struct TypingState {
    std::string roomId;
    std::vector<TypingUser> typingUsers;
    int typingCount = 0;
    int64_t snapshotTs = 0;
    bool hasAnyoneTyping = false;
};

// Timeout after which a user is considered not typing (ms).
static constexpr int64_t TYPING_TIMEOUT_MS = 30000;   // 30 seconds

// Merge new typing notification into existing state.
// Original Kotlin (TypingUsersTracker.kt):
//   fun onTypingUsersUpdate(roomId: String, userIds: List<String>, now: Long)
TypingState updateTypingState(
    const std::string& roomId,
    const std::vector<std::string>& currentTypingUserIds,
    const std::vector<std::string>& displayNames,
    int64_t nowMs
);

// Check if a specific user is still typing (timeout check).
bool isUserTyping(const TypingState& state, const std::string& userId, int64_t nowMs);

// Prune expired typing users from the state.
TypingState pruneTypingUsers(const TypingState& state, int64_t nowMs);

// Format typing indicator text for display.
// "Alice is typing..."
// "Alice and Bob are typing..."
// "Alice, Bob and 3 others are typing..."
// Original Kotlin (TypingHelper.kt):
//   fun formatTypingText(typingUsers: List<String>, maxNames: Int): String
std::string formatTypingText(const TypingState& state, int maxNames = 3);

// Format typing state as JSON for the Kotlin UI layer.
std::string typingStateToJson(const TypingState& state);

} // namespace progressive

#endif // PROGRESSIVE_TYPING_MONITOR_HPP

// src/core/engine/engine_types.hpp — Qt-free state-layer types (X1).
// The source of truth for the UI models: they become thin projections of
// this state. NO Qt types here (CI guard: scripts/check_no_qt_in_core.sh).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace progressive::desktop {

// One reaction on a message (m.reaction).
struct ReactionData {
    std::string emoji;
    int count = 0;
    bool addedByMe = false;
    std::vector<std::string> userIds;
    std::string reactionEventId;  // event_id of the m.reaction event (for redaction)
};

// One rendered timeline event. QString/QImage live ONLY in the view layer;
// images are resolved via the ImageLoader cache by mxcUrl (Option B).
struct DisplayedEvent {
    std::string eventId;
    std::string senderId;
    std::string senderName;        // displayname or localpart
    std::string type;              // m.room.message, m.room.member, etc.
    std::string msgtype;           // m.text, m.image, m.emote, etc.
    std::string body;              // text body (for text messages)
    std::string contentJson;       // raw content JSON (for images, etc.)
    std::string mxcUrl;            // for images: mxc:// URL
    std::string mimetype;          // for images: image/gif, image/png, etc.
    // Encrypted media (file: object, m.encrypted v2): base64 key/iv + sha256
    // of the plaintext. Empty for plain media. thumb* = info.thumbnail_file.
    std::string mediaKey;
    std::string mediaIv;
    std::string mediaSha256;
    std::string thumbUrl;
    std::string thumbKey;
    std::string thumbIv;
    std::string thumbSha256;
    // Why an encrypted message could not be decrypted ("" = decrypted/plain).
    // Surfaced in the UI (badge + tooltip) so "why is this encrypted?" is
    // answerable in-app without console logs.
    std::string decryptError;
    int64_t originServerTs = 0;
    bool isReply = false;          // has m.relates_to m.reply
    std::string replyToEventId;    // if isReply
    bool isReplace = false;        // m.relates_to m.replace (an edit)
    std::string replaceTargetId;   // event being edited
    bool isThreadRoot = false;     // has m.thread replies
    int threadReplyCount = 0;
    bool isThreadReply = false;    // this message is a reply in a thread
    std::string threadRootId;      // root event_id if isThreadReply
    bool isDateDivider = false;
    std::string dividerLabel;
    bool isPinned = false;
    bool groupFirst = true;         // first message in same-sender group
    bool groupLast  = true;         // last message in same-sender group
    enum Delivery { Sending, Delivered, Failed };
    Delivery delivery = Delivered;  // delivery state for own messages
    std::vector<ReactionData> reactions;
    bool isMovie = false;          // true for animated GIF (currently dead — never set)
    std::string avatarUrl;         // sender's avatar mxc URL (from m.room.member)
};

// One room in the room list.
struct RoomData {
    std::string roomId;
    std::string name;
    std::string lastMessage;
    std::string lastSender;
    int64_t lastActivityTs = 0;
    int unreadCount = 0;
    int highlightCount = 0;
    bool isDirect = false;
    bool isEncrypted = false;
    bool isSpace = false;
    bool isInvite = false;
    std::string inviterId;
    int memberCount = 0;
    std::string avatarUrl;
    std::string parentId;
    std::vector<std::string> typingUsers;  // users currently typing
    bool stateLoaded = false;  // m.room.encryption state already fetched for this room
};

} // namespace progressive::desktop

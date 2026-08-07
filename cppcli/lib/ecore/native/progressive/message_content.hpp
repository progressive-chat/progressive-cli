#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace progressive {

// ==== Message Content Interface ====
//
// Original Kotlin (EventMessageContent.kt:25-33):
//   interface EventMessageContent {
//       val msgType: String
//       val body: String
//       val relatesTo: RelationDefaultContent?
//       val newContent: Content?
//   }

struct RelationDefaultContent {
    std::string eventId;       // "$in_reply_to" or "$m.annotation"
    std::string relationType;  // "m.in_reply_to", "m.annotation", "m.replace", "m.reference"
    std::string key;           // For m.annotation
    bool isFallingBack = false;
};

// Base message content — all messages have these fields.
// Original Kotlin: interface EventMessageContent
struct EventMessageContent {
    std::string msgType;              // "msgtype" key
    std::string body;                 // "body" key — required human-readable text
    RelationDefaultContent relatesTo; // "m.relates_to" key
    std::string relatesToRaw;         // JNI compat: raw JSON for relates_to
    std::string newContent;           // "m.new_content" key (for edits) — raw JSON or empty
    bool hasRelation = false;
    bool isEdit = false;
    bool isFallback = false;          // convenience: JNI flat access (from is_fallback JSON)
};

// ==== Formatted Body ====
//
// Original Kotlin (MessageContentWithFormattedBody.kt:24-35):
//   interface MessageContentWithFormattedBody : EventMessageContent {
//       val format: String?
//       val formattedBody: String?
//       val matrixFormattedBody: String?
//           get() = formattedBody?.takeIf {
//               it.isNotBlank() && format == MessageFormat.FORMAT_MATRIX_HTML
//           }
//   }

struct MessageContentWithFormattedBody : EventMessageContent {
    std::string format;        // "format" key, e.g. "org.matrix.custom.html"
    std::string formattedBody; // "formatted_body" key

    // Original Kotlin: get matrixFormattedBody
    std::string getMatrixFormattedBody() const {
        if (formattedBody.empty()) return "";
        if (format != "org.matrix.custom.html") return "";
        return formattedBody;
    }
};

// ==== Text-based Message Types ====
//
// Original Kotlin (MessageTextContent.kt:26-43), (MessageNoticeContent.kt:26-43), (MessageEmoteContent.kt:26-43):
//   data class MessageTextContent(@Json(name="msgtype") msgType, @Json(name="body") body,
//       @Json(name="format") format, @Json(name="formatted_body") formattedBody,
//       @Json(name="m.relates_to") relatesTo, @Json(name="m.new_content") newContent)
//   : MessageContentWithFormattedBody
//
// JSON: {"msgtype":"m.text","body":"Hello","format":"org.matrix.custom.html",
//        "formatted_body":"<b>Hello</b>"}

struct MessageTextContent : MessageContentWithFormattedBody {
    // Same fields as MessageContentWithFormattedBody
    // msgType = "m.text"
};

struct MessageNoticeContent : MessageContentWithFormattedBody {
    // Same fields, msgType = "m.notice"
};

struct MessageEmoteContent : MessageContentWithFormattedBody {
    // Same fields, msgType = "m.emote"
};

// ==== Attachment Info Types ====
//
// Original Kotlin (ImageInfo.kt:25-55): width, height, size, mimeType, thumbnailInfo, thumbnailUrl, thumbnailFile
// Original Kotlin (VideoInfo.kt:25-56): adds duration
// Original Kotlin (AudioInfo.kt:23-36): mimeType, size, duration
// Original Kotlin (FileInfo.kt:26-46): mimeType, size, thumbnailInfo, thumbnailUrl, thumbnailFile
// Original Kotlin (ThumbnailInfo.kt:24-40): width, height, size, mimeType

struct EncryptedFileInfo {
    std::string url;       // MXC URI of encrypted file
    std::string keyJson;   // JWK key JSON
    std::string iv;        // Initialisation vector
    std::string hashes;    // JSON: {"sha256":"base64..."}
    std::string version;   // "v2"
    // JWK convenience fields (JNI flat access from EncryptedFileKey)
    std::string alg;
    std::string kty;
    std::string k;
    std::string key;
    std::string ext;
    std::string keyOps;
};

struct ThumbnailInfo {
    int width = 0;
    int height = 0;
    int64_t size = 0;
    std::string mimeType;        // e.g. "image/jpeg"

    bool hasData() const { return width > 0 || height > 0 || size > 0; }
};

struct ImageInfo {
    std::string mimeType;        // e.g. "image/jpeg"
    int width = 0;
    int height = 0;
    int64_t size = 0;
    ThumbnailInfo thumbnailInfo;
    std::string thumbnailUrl;    // MXC URI or empty
    EncryptedFileInfo thumbnailFile; // if encrypted

    // Original Kotlin (ImageInfo.kt:57-59):
    //   fun getThumbnailUrl(): String? = thumbnailFile?.url ?: thumbnailUrl
    std::string getThumbnailUrl() const {
        if (!thumbnailFile.url.empty()) return thumbnailFile.url;
        return thumbnailUrl;
    }
};

struct VideoInfo {
    std::string mimeType;        // e.g. "video/mp4"
    int width = 0;
    int height = 0;
    int64_t size = 0;
    int duration = 0;            // milliseconds
    ThumbnailInfo thumbnailInfo;
    std::string thumbnailUrl;
    EncryptedFileInfo thumbnailFile;

    std::string getThumbnailUrl() const {
        // Original Kotlin (VideoInfo.kt:58-60):
        //   fun getThumbnailUrl(): String? = thumbnailFile?.url ?: thumbnailUrl
        if (!thumbnailFile.url.empty()) return thumbnailFile.url;
        return thumbnailUrl;
    }
};

struct AudioInfo {
    std::string mimeType;        // e.g. "audio/aac"
    int64_t size = 0;
    int duration = 0;            // milliseconds
};

struct FileInfo {
    std::string mimeType;        // e.g. "application/pdf"
    int64_t size = 0;
    ThumbnailInfo thumbnailInfo;
    std::string thumbnailUrl;
    EncryptedFileInfo thumbnailFile;

    std::string getThumbnailUrl() const {
        // Original Kotlin (FileInfo.kt:48-50):
        //   fun getThumbnailUrl(): String? = thumbnailFile?.url ?: thumbnailUrl
        if (!thumbnailFile.url.empty()) return thumbnailFile.url;
        return thumbnailUrl;
    }
};

struct AudioWaveformInfo {
    std::vector<int> waveform;   // Array of ints for waveform visualization
    int duration = 0;            // milliseconds
};

// ==== Media Message Types ====
//
// Original Kotlin (MessageImageContent.kt:28-55), (MessageVideoContent.kt:28-56),
//   (MessageAudioContent.kt:28-63), (MessageFileContent.kt:28-57)

struct MessageImageContent : EventMessageContent {
    ImageInfo info;              // "info" key
    std::string url;             // "url" key — MXC URI (unencrypted)
    EncryptedFileInfo encryptedFile; // "file" key
    std::string mimeType;        // derived: info.mimeType
    std::string thumbnailUrl;    // convenience: JNI flat access
    std::string filename;        // convenience: JNI flat access
    ThumbnailInfo thumbnailInfo; // convenience: JNI flat access
    int width = 0;               // convenience
    int height = 0;              // convenience
    int64_t size = 0;            // convenience

    // Original Kotlin: getFileUrl() = encryptedFile?.url ?: url
    std::string getFileUrl() const {
        if (!encryptedFile.url.empty()) return encryptedFile.url;
        return url;
    }
};

struct MessageVideoContent : EventMessageContent {
    VideoInfo videoInfo;         // "info" key
    std::string url;
    EncryptedFileInfo encryptedFile;
    std::string mimeType;        // derived: videoInfo.mimeType
    std::string thumbnailUrl;    // convenience
    std::string filename;        // convenience
    int64_t duration = 0;        // convenience
    int width = 0;               // convenience
    int height = 0;              // convenience
    int64_t size = 0;            // convenience

    std::string getFileUrl() const {
        if (!encryptedFile.url.empty()) return encryptedFile.url;
        return url;
    }
};

struct MessageAudioContent : EventMessageContent {
    AudioInfo audioInfo;         // "info" key
    std::string url;
    EncryptedFileInfo encryptedFile;
    AudioWaveformInfo audioWaveform;  // "org.matrix.msc1767.audio" key
    bool isVoiceMessage = false;      // "org.matrix.msc3245.voice" key presence
    std::string mimeType;        // derived: audioInfo.mimeType
    std::string filename;        // convenience
    int64_t duration = 0;        // convenience
    int64_t size = 0;            // convenience

    std::string getFileUrl() const {
        if (!encryptedFile.url.empty()) return encryptedFile.url;
        return url;
    }
};

struct MessageFileContent : EventMessageContent {
    std::string filename;        // "filename" key
    FileInfo info;               // "info" key
    std::string url;
    EncryptedFileInfo encryptedFile;
    std::string mimeType;        // derived: info.mimeType or extension-based
    int64_t size = 0;            // convenience

    std::string getFileUrl() const {
        if (!encryptedFile.url.empty()) return encryptedFile.url;
        return url;
    }

    // Original Kotlin (MessageFileContent.kt:62-64):
    //   fun getFileName(): String = filename ?: body
    std::string getFileName() const {
        return !filename.empty() ? filename : body;
    }
};

struct MessageLocationContent : EventMessageContent {
    std::string geoUri;          // "geo_uri" key
    std::string mxcUrl;          // "url" key for location image
    int64_t latitudeE7 = 0;      // from geo_uri parsing
    int64_t longitudeE7 = 0;
};

// ==== JSON Parsing Functions ====

// Parse a message content struct from event content JSON.
// Returns the typed message based on msgtype field.
enum class ParsedMessageType {
    UNKNOWN, TEXT, NOTICE, EMOTE, IMAGE, VIDEO, AUDIO, FILE, LOCATION,
    STICKER, POLL_START, POLL_RESPONSE, POLL_END
};

struct ParsedMessage {
    ParsedMessageType type = ParsedMessageType::UNKNOWN;
    MessageTextContent text;
    MessageNoticeContent notice;
    MessageEmoteContent emote;
    MessageImageContent image;
    MessageVideoContent video;
    MessageAudioContent audio;
    MessageFileContent file;
    MessageLocationContent location;
    std::string rawJson;         // Raw content JSON
};

// Parse message content from JSON
ParsedMessage parseMessageFull(const std::string& contentJson);

// Serialize message content structs to JSON
std::string messageTextToJson(const MessageTextContent& msg);
std::string messageNoticeToJson(const MessageNoticeContent& msg);
std::string messageEmoteToJson(const MessageEmoteContent& msg);
std::string messageImageToJson(const MessageImageContent& msg);
std::string messageVideoToJson(const MessageVideoContent& msg);
std::string messageAudioToJson(const MessageAudioContent& msg);
std::string messageFileToJson(const MessageFileContent& msg);

// Determine message type from msgtype string
ParsedMessageType msgTypeFromString(const std::string& msgtype);


enum class ContentType {
    TEXT, NOTICE, EMOTE, IMAGE, VIDEO, AUDIO, FILE, LOCATION,
    POLL_START, POLL_RESPONSE, POLL_END, STICKER,
    BEACON_INFO, BEACON_LOCATION_DATA,
    VERIFICATION_REQUEST, VERIFICATION_START, VERIFICATION_DONE,
    VERIFICATION_CANCEL, VERIFICATION_READY, VERIFICATION_KEY, VERIFICATION_MAC,
    CALL_INVITE, CALL_ANSWER, CALL_HANGUP, CALL_REJECT,
    CALL_CANDIDATES, CALL_NEGOTIATE, CALL_SELECT_ANSWER,
    VOICE_BROADCAST_INFO, STATE_EVENT, UNKNOWN
};

// Original Kotlin: MessageContentStats
struct MessageContentStats {
    int textCount = 0;
    int imageCount = 0;
    int videoCount = 0;
    int audioCount = 0;
    int fileCount = 0;
    int locationCount = 0;
    int pollCount = 0;
    int stickerCount = 0;
    int callCount = 0;
    int verificationCount = 0;
    int otherCount = 0;
    int totalCount = 0;
};

// Original Kotlin: MessageContentTypeDetector
class MessageContentTypeDetector {
public:
    // Detect the content type from raw event content JSON.
    // Original Kotlin: detectContentType()
    static ContentType detectContentType(const std::string& contentJson);

    // Get a human-readable description for a content type.
    // Original Kotlin: getContentTypeDescription()
    static const char* getContentTypeDescription(ContentType type);

    // Check if a content type is displayable in the timeline.
    // Original Kotlin: isContentTypeDisplayable()
    static bool isContentTypeDisplayable(ContentType type);

    // Get an icon resource name for a content type.
    // Original Kotlin: getContentTypeIcon()
    static const char* getContentTypeIcon(ContentType type);
};

// Compute statistics across a set of message content JSONs.
// Original Kotlin: computeMessageContentStats()
MessageContentStats computeMessageContentStats(const std::vector<std::string>& contentJsons);

} // namespace progressive

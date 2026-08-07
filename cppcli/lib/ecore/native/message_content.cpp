#include "progressive/message_content.hpp"
#include <cstring>

namespace progressive {

// ==== JSON Extraction Helpers ====

static std::string extractJsonString(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    size_t end = pos;
    while (end < json.size() && json[end] != '"') {
        if (json[end] == '\\') end++;
        end++;
    }
    return json.substr(pos, end - pos);
}

static int extractJsonInt(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return 0;
    int val = 0;
    bool negative = false;
    if (json[pos] == '-') { negative = true; pos++; }
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return negative ? -val : val;
}

static int64_t extractJsonInt64(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return 0;
    int64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++;
    }
    return val;
}

static std::string extractJsonObject(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '{') return "";
    int depth = 1;
    size_t start = pos;
    pos++;
    while (pos < json.size() && depth > 0) {
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') depth--;
        pos++;
    }
    return json.substr(start, pos - start);
}

// ==== Parse Info Types ====

// Original Kotlin (ThumbnailInfo.kt:24-40)
static ThumbnailInfo parseThumbnailInfo(const std::string& json) {
    ThumbnailInfo ti;
    ti.width = extractJsonInt(json, "w");
    ti.height = extractJsonInt(json, "h");
    ti.size = extractJsonInt64(json, "size");
    ti.mimeType = extractJsonString(json, "mimetype");
    return ti;
}

// Original Kotlin (ImageInfo.kt:25-55)
static ImageInfo parseImageInfo(const std::string& json) {
    ImageInfo info;
    info.mimeType = extractJsonString(json, "mimetype");
    info.width = extractJsonInt(json, "w");
    info.height = extractJsonInt(json, "h");
    info.size = extractJsonInt64(json, "size");
    auto tnJson = extractJsonObject(json, "thumbnail_info");
    if (!tnJson.empty()) info.thumbnailInfo = parseThumbnailInfo(tnJson);
    info.thumbnailUrl = extractJsonString(json, "thumbnail_url");
    // thumbnail_file handled separately if needed
    return info;
}

// Original Kotlin (VideoInfo.kt:25-56)
static VideoInfo parseVideoInfo(const std::string& json) {
    VideoInfo info;
    info.mimeType = extractJsonString(json, "mimetype");
    info.width = extractJsonInt(json, "w");
    info.height = extractJsonInt(json, "h");
    info.size = extractJsonInt64(json, "size");
    info.duration = extractJsonInt(json, "duration");
    auto tnJson = extractJsonObject(json, "thumbnail_info");
    if (!tnJson.empty()) info.thumbnailInfo = parseThumbnailInfo(tnJson);
    info.thumbnailUrl = extractJsonString(json, "thumbnail_url");
    return info;
}

// Original Kotlin (AudioInfo.kt:23-36)
static AudioInfo parseAudioInfo(const std::string& json) {
    AudioInfo info;
    info.mimeType = extractJsonString(json, "mimetype");
    info.size = extractJsonInt64(json, "size");
    info.duration = extractJsonInt(json, "duration");
    return info;
}

// Original Kotlin (FileInfo.kt:26-46)
static FileInfo parseFileInfo(const std::string& json) {
    FileInfo info;
    info.mimeType = extractJsonString(json, "mimetype");
    info.size = extractJsonInt64(json, "size");
    auto tnJson = extractJsonObject(json, "thumbnail_info");
    if (!tnJson.empty()) info.thumbnailInfo = parseThumbnailInfo(tnJson);
    info.thumbnailUrl = extractJsonString(json, "thumbnail_url");
    return info;
}

// ==== Parse Relation ====

static RelationDefaultContent parseRelation(const std::string& contentJson) {
    RelationDefaultContent rel;
    auto relJson = extractJsonObject(contentJson, "m.relates_to");
    if (relJson.empty()) return rel;
    rel.eventId = extractJsonString(relJson, "event_id");
    rel.relationType = extractJsonString(relJson, "rel_type");
    rel.key = extractJsonString(relJson, "key");
    return rel;
}

// ==== Message Type Detection ====
//
// Original Kotlin (MessageType.kt:22-58):
//   object MessageType {
//       const val MSGTYPE_TEXT = "m.text", MSGTYPE_EMOTE = "m.emote", etc.
//   }

ParsedMessageType msgTypeFromString(const std::string& msgtype) {
    if (msgtype == "m.text") return ParsedMessageType::TEXT;
    if (msgtype == "m.notice") return ParsedMessageType::NOTICE;
    if (msgtype == "m.emote") return ParsedMessageType::EMOTE;
    if (msgtype == "m.image") return ParsedMessageType::IMAGE;
    if (msgtype == "m.video") return ParsedMessageType::VIDEO;
    if (msgtype == "m.audio") return ParsedMessageType::AUDIO;
    if (msgtype == "m.file") return ParsedMessageType::FILE;
    if (msgtype == "m.location") return ParsedMessageType::LOCATION;
    // Original Kotlin: local fake types
    if (msgtype == "org.matrix.android.sdk.sticker") return ParsedMessageType::STICKER;
    if (msgtype == "org.matrix.android.sdk.poll.start") return ParsedMessageType::POLL_START;
    if (msgtype == "org.matrix.android.sdk.poll.response") return ParsedMessageType::POLL_RESPONSE;
    if (msgtype == "org.matrix.android.sdk.poll.end") return ParsedMessageType::POLL_END;
    return ParsedMessageType::UNKNOWN;
}

// ==== Main Message Parser ====
//
// Original Kotlin: parse the "content" JSON blob from an event.
// Each message type is parsed by Moshi from the same JSON structure,
// differentiated by the "msgtype" field.

void fillMessageContent(EventMessageContent& mc, const std::string& json) {
    mc.msgType = extractJsonString(json, "msgtype");
    mc.body = extractJsonString(json, "body");
    mc.relatesTo = parseRelation(json);
    mc.hasRelation = !mc.relatesTo.eventId.empty();
    mc.newContent = extractJsonObject(json, "m.new_content");
    mc.isEdit = !mc.newContent.empty();
}

void fillFormattedBody(MessageContentWithFormattedBody& fmt, const std::string& json) {
    fillMessageContent(fmt, json);
    fmt.format = extractJsonString(json, "format");
    fmt.formattedBody = extractJsonString(json, "formatted_body");
}

ParsedMessage parseMessageContent(const std::string& contentJson) {
    ParsedMessage result;
    result.rawJson = contentJson;

    auto msgtype = extractJsonString(contentJson, "msgtype");
    result.type = msgTypeFromString(msgtype);

    switch (result.type) {
        // Original Kotlin (MessageTextContent.kt:26-43)
        case ParsedMessageType::TEXT:
            fillFormattedBody(result.text, contentJson);
            break;
        // Original Kotlin (MessageNoticeContent.kt:26-43)
        case ParsedMessageType::NOTICE:
            fillFormattedBody(result.notice, contentJson);
            break;
        // Original Kotlin (MessageEmoteContent.kt:26-43)
        case ParsedMessageType::EMOTE:
            fillFormattedBody(result.emote, contentJson);
            break;
        // Original Kotlin (MessageImageContent.kt:28-55)
        case ParsedMessageType::IMAGE:
            fillMessageContent(result.image, contentJson);
            result.image.url = extractJsonString(contentJson, "url");
            {
                auto infoJson = extractJsonObject(contentJson, "info");
                if (!infoJson.empty()) result.image.info = parseImageInfo(infoJson);
                result.image.mimeType = result.image.info.mimeType;
            }
            break;
        // Original Kotlin (MessageVideoContent.kt:28-56)
        case ParsedMessageType::VIDEO:
            fillMessageContent(result.video, contentJson);
            result.video.url = extractJsonString(contentJson, "url");
            {
                auto infoJson = extractJsonObject(contentJson, "info");
                if (!infoJson.empty()) result.video.videoInfo = parseVideoInfo(infoJson);
                result.video.mimeType = result.video.videoInfo.mimeType;
            }
            break;
        // Original Kotlin (MessageAudioContent.kt:28-63)
        case ParsedMessageType::AUDIO:
            fillMessageContent(result.audio, contentJson);
            result.audio.url = extractJsonString(contentJson, "url");
            {
                auto infoJson = extractJsonObject(contentJson, "info");
                if (!infoJson.empty()) result.audio.audioInfo = parseAudioInfo(infoJson);
                result.audio.mimeType = result.audio.audioInfo.mimeType;
            }
            result.audio.isVoiceMessage = contentJson.find("\"org.matrix.msc3245.voice\"") != std::string::npos;
            break;
        // Original Kotlin (MessageFileContent.kt:28-57)
        case ParsedMessageType::FILE:
            fillMessageContent(result.file, contentJson);
            result.file.filename = extractJsonString(contentJson, "filename");
            result.file.url = extractJsonString(contentJson, "url");
            {
                auto infoJson = extractJsonObject(contentJson, "info");
                if (!infoJson.empty()) result.file.info = parseFileInfo(infoJson);
                result.file.mimeType = result.file.info.mimeType;
            }
            break;
        // Original Kotlin (MessageLocationContent)
        case ParsedMessageType::LOCATION:
            fillMessageContent(result.location, contentJson);
            result.location.geoUri = extractJsonString(contentJson, "geo_uri");
            result.location.mxcUrl = extractJsonString(contentJson, "url");
            break;
        default:
            break;
    }

    return result;
}

// ==== JSON Serialization ====

static std::string escapeJson(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    r += "\"";
    return r;
}

// Original Kotlin (MessageTextContent.kt:26-43), Moshi-generated JSON
std::string messageTextToJson(const MessageTextContent& msg) {
    std::string json = "{";
    json += "\"msgtype\":" + escapeJson(msg.msgType) + ",";
    json += "\"body\":" + escapeJson(msg.body);
    // Original Kotlin: format / formatted_body
    if (!msg.format.empty()) {
        json += ",\"format\":" + escapeJson(msg.format);
        if (!msg.formattedBody.empty()) {
            json += ",\"formatted_body\":" + escapeJson(msg.formattedBody);
        }
    }
    // Original Kotlin: relates_to
    if (msg.hasRelation) {
        json += ",\"m.relates_to\":{\"event_id\":" + escapeJson(msg.relatesTo.eventId);
        if (!msg.relatesTo.relationType.empty())
            json += ",\"rel_type\":" + escapeJson(msg.relatesTo.relationType);
        json += "}";
    }
    if (!msg.newContent.empty()) {
        json += ",\"m.new_content\":" + msg.newContent;
    }
    json += "}";
    return json;
}

std::string messageNoticeToJson(const MessageNoticeContent& msg) {
    auto base = reinterpret_cast<const MessageContentWithFormattedBody*>(&msg);
    return messageTextToJson(reinterpret_cast<const MessageTextContent&>(*base));
}

std::string messageEmoteToJson(const MessageEmoteContent& msg) {
    auto base = reinterpret_cast<const MessageContentWithFormattedBody*>(&msg);
    return messageTextToJson(reinterpret_cast<const MessageTextContent&>(*base));
}

std::string messageImageToJson(const MessageImageContent& msg) {
    std::string json = "{";
    json += "\"msgtype\":" + escapeJson(msg.msgType) + ",";
    json += "\"body\":" + escapeJson(msg.body);
    if (!msg.url.empty()) json += ",\"url\":" + escapeJson(msg.url);
    // info
    if (msg.info.width > 0 || msg.info.height > 0 || msg.info.size > 0) {
        json += ",\"info\":{";
        if (!msg.info.mimeType.empty()) json += "\"mimetype\":" + escapeJson(msg.info.mimeType) + ",";
        json += "\"w\":" + std::to_string(msg.info.width) + ",";
        json += "\"h\":" + std::to_string(msg.info.height) + ",";
        json += "\"size\":" + std::to_string(msg.info.size);
        if (msg.info.thumbnailInfo.hasData()) {
            json += ",\"thumbnail_info\":{\"w\":" + std::to_string(msg.info.thumbnailInfo.width) +
                    ",\"h\":" + std::to_string(msg.info.thumbnailInfo.height) +
                    ",\"size\":" + std::to_string(msg.info.thumbnailInfo.size) + "}";
        }
        json += "}";
    }
    if (msg.hasRelation) {
        json += ",\"m.relates_to\":{\"event_id\":" + escapeJson(msg.relatesTo.eventId) + "}";
    }
    json += "}";
    return json;
}

std::string messageVideoToJson(const MessageVideoContent& msg) {
    std::string json = "{";
    json += "\"msgtype\":" + escapeJson(msg.msgType) + ",";
    json += "\"body\":" + escapeJson(msg.body);
    if (!msg.url.empty()) json += ",\"url\":" + escapeJson(msg.url);
    // info (videoInfo)
    json += ",\"info\":{";
    if (!msg.videoInfo.mimeType.empty()) json += "\"mimetype\":" + escapeJson(msg.videoInfo.mimeType) + ",";
    json += "\"w\":" + std::to_string(msg.videoInfo.width) + ",";
    json += "\"h\":" + std::to_string(msg.videoInfo.height) + ",";
    json += "\"size\":" + std::to_string(msg.videoInfo.size) + ",";
    json += "\"duration\":" + std::to_string(msg.videoInfo.duration);
    json += "}";
    if (msg.hasRelation) {
        json += ",\"m.relates_to\":{\"event_id\":" + escapeJson(msg.relatesTo.eventId) + "}";
    }
    json += "}";
    return json;
}

std::string messageAudioToJson(const MessageAudioContent& msg) {
    std::string json = "{";
    json += "\"msgtype\":" + escapeJson(msg.msgType) + ",";
    json += "\"body\":" + escapeJson(msg.body);
    if (!msg.url.empty()) json += ",\"url\":" + escapeJson(msg.url);
    // Original Kotlin: voice message indicator
    if (msg.isVoiceMessage) {
        json += ",\"org.matrix.msc3245.voice\":{}";
    }
    json += "}";
    return json;
}

std::string messageFileToJson(const MessageFileContent& msg) {
    std::string json = "{";
    json += "\"msgtype\":" + escapeJson(msg.msgType) + ",";
    json += "\"body\":" + escapeJson(msg.body);
    if (!msg.filename.empty()) json += ",\"filename\":" + escapeJson(msg.filename);
    if (!msg.url.empty()) json += ",\"url\":" + escapeJson(msg.url);
    if (msg.info.size > 0 || !msg.info.mimeType.empty()) {
        json += ",\"info\":{";
        if (!msg.info.mimeType.empty()) json += "\"mimetype\":" + escapeJson(msg.info.mimeType) + ",";
        json += "\"size\":" + std::to_string(msg.info.size);
        json += "}";
    }
    json += "}";
    return json;
}



// ---- ContentType detection ----

ContentType MessageContentTypeDetector::detectContentType(const std::string& contentJson) {
    if (contentJson.empty()) return ContentType::UNKNOWN;

    auto hasType = [&](const std::string& t) { return contentJson.find("\"msgtype\":\"" + t + "\"") != std::string::npos; };
    auto hasEvent = [&](const std::string& t) { return contentJson.find(t) != std::string::npos; };

    if (hasType("m.text")) return ContentType::TEXT;
    if (hasType("m.notice")) return ContentType::NOTICE;
    if (hasType("m.emote")) return ContentType::EMOTE;
    if (hasType("m.image")) return ContentType::IMAGE;
    if (hasType("m.video")) return ContentType::VIDEO;
    if (hasType("m.audio")) return ContentType::AUDIO;
    if (hasType("m.file")) return ContentType::FILE;
    if (hasType("m.location")) return ContentType::LOCATION;
    if (hasEvent("poll.start") || hasEvent("m.poll.start")) return ContentType::POLL_START;
    if (hasEvent("poll.response") || hasEvent("m.poll.response")) return ContentType::POLL_RESPONSE;
    if (hasEvent("poll.end") || hasEvent("m.poll.end")) return ContentType::POLL_END;
    if (hasType("m.sticker")) return ContentType::STICKER;
    if (hasEvent("m.call.invite")) return ContentType::CALL_INVITE;
    if (hasEvent("m.call.answer")) return ContentType::CALL_ANSWER;
    if (hasEvent("m.call.hangup")) return ContentType::CALL_HANGUP;
    if (hasEvent("m.call.reject")) return ContentType::CALL_REJECT;
    if (hasEvent("m.call.candidates")) return ContentType::CALL_CANDIDATES;
    if (hasEvent("m.key.verification")) return ContentType::VERIFICATION_REQUEST;
    if (hasEvent("io.element.voice_broadcast_info")) return ContentType::VOICE_BROADCAST_INFO;
    if (contentJson.find("\"state_key\"") != std::string::npos) return ContentType::STATE_EVENT;
    return ContentType::UNKNOWN;
}

MessageContentStats computeContentStats(const std::vector<std::string>& eventBodies) {
    MessageContentStats stats;
    for (const auto& body : eventBodies) {
        auto t = MessageContentTypeDetector::detectContentType(body);
        switch (t) {
            case ContentType::TEXT:     stats.textCount++; break;
            case ContentType::NOTICE:   stats.textCount++; break;
            case ContentType::EMOTE:    stats.textCount++; break;
            case ContentType::IMAGE:    stats.imageCount++; break;
            case ContentType::VIDEO:    stats.videoCount++; break;
            case ContentType::AUDIO:    stats.audioCount++; break;
            case ContentType::FILE:     stats.fileCount++; break;
            case ContentType::LOCATION: stats.locationCount++; break;
            case ContentType::POLL_START: case ContentType::POLL_RESPONSE: case ContentType::POLL_END: stats.pollCount++; break;
            case ContentType::STICKER:  stats.stickerCount++; break;
            case ContentType::CALL_INVITE: case ContentType::CALL_ANSWER: case ContentType::CALL_HANGUP:
            case ContentType::CALL_REJECT: case ContentType::CALL_CANDIDATES: stats.callCount++; break;
            case ContentType::VERIFICATION_REQUEST: case ContentType::VERIFICATION_DONE:
                stats.verificationCount++; break;
            default: stats.otherCount++; break;
        }
        stats.totalCount++;
    }
    return stats;
}

} // namespace progressive

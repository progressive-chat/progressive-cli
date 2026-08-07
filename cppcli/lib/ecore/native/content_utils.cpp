#include "progressive/content_utils.hpp"
#include "progressive/string_utils.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace progressive {

// ---- MXC URL Resolution ----
// Original Kotlin (ContentUrlResolver.kt):
//   class ContentUrlResolver(private val homeServerUrl: String) {
//       fun resolveDownloadUrl(mxc: String): String {
//           val parts = mxc.removePrefix("mxc://").split("/")
//           return "$homeServerUrl/_matrix/media/v3/download/${parts[0]}/${parts[1]}"
//       }
//   }

std::string resolveMxcDownloadUrl(const std::string& mxcUrl, const std::string& homeServerUrl) {
    if (!isMxcUri(mxcUrl)) return mxcUrl; // already an HTTP URL

    auto server = extractMxcServerName(mxcUrl);
    auto mediaId = extractMxcMediaId(mxcUrl);
    if (server.empty() || mediaId.empty()) return mxcUrl;

    // Matrix spec: GET /_matrix/media/v3/download/{serverName}/{mediaId}
    std::string base = homeServerUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/_matrix/media/v3/download/" + server + "/" + mediaId;
}

std::string resolveMxcThumbnailUrl(const std::string& mxcUrl, const std::string& homeServerUrl,
    int width, int height, const std::string& method) {
    if (!isMxcUri(mxcUrl)) return mxcUrl;

    auto server = extractMxcServerName(mxcUrl);
    auto mediaId = extractMxcMediaId(mxcUrl);
    if (server.empty() || mediaId.empty()) return mxcUrl;

    std::string base = homeServerUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();

    // Matrix spec: GET /_matrix/media/v3/thumbnail/{serverName}/{mediaId}?w=...&h=...&method=...
    std::ostringstream out;
    out << base << "/_matrix/media/v3/thumbnail/" << server << "/" << mediaId;
    out << "?width=" << width << "&height=" << height << "&method=" << method;
    return out.str();
}

bool isMxcUri(const std::string& url) {
    return url.find("mxc://") == 0;
}

std::string extractMxcServerName(const std::string& mxcUrl) {
    // "mxc://server.name/media_id"
    auto prefix = std::string("mxc://");
    if (mxcUrl.find(prefix) != 0) return "";
    auto start = prefix.size();
    auto slashPos = mxcUrl.find('/', start);
    if (slashPos == std::string::npos) return "";
    return mxcUrl.substr(start, slashPos - start);
}

std::string extractMxcMediaId(const std::string& mxcUrl) {
    auto prefix = std::string("mxc://");
    if (mxcUrl.find(prefix) != 0) return "";
    auto start = prefix.size();
    auto slashPos = mxcUrl.find('/', start);
    if (slashPos == std::string::npos) return "";
    return mxcUrl.substr(slashPos + 1);
}

std::string buildMxcUri(const std::string& serverName, const std::string& mediaId) {
    return "mxc://" + serverName + "/" + mediaId;
}

// ---- Message Content Parsing ----
// Original Kotlin (MessageContent.kt):
//   fun parse(content: Content): MessageContent {
//       val msgtype = content.get("msgtype")?.asString()
//       return when (msgtype) {
//           "m.text" -> TextContent(...)
//           "m.image" -> ImageContent(...)
//           ...
//       }
//   }

MessageType parseMessageType(const std::string& contentJson) {
    // Extract msgtype field:
    // {"msgtype":"m.text","body":"Hello"}
    const std::string search = "\"msgtype\":\"";
    auto pos = contentJson.find(search);
    if (pos == std::string::npos) {
        const std::string search2 = "\"msgtype\": \"";
        pos = contentJson.find(search2);
        if (pos == std::string::npos) return MessageType::Unknown;
        pos += search2.size();
    } else {
        pos += search.size();
    }
    auto end = contentJson.find('"', pos);
    if (end == std::string::npos) return MessageType::Unknown;

    std::string msgtype = contentJson.substr(pos, end - pos);

    if (msgtype == "m.text") return MessageType::Text;
    if (msgtype == "m.notice") return MessageType::Notice;
    if (msgtype == "m.emote") return MessageType::Emote;
    if (msgtype == "m.image") return MessageType::Image;
    if (msgtype == "m.video") return MessageType::Video;
    if (msgtype == "m.audio") return MessageType::Audio;
    if (msgtype == "m.file") return MessageType::File;
    if (msgtype == "m.location") return MessageType::Location;
    if (msgtype == "m.sticker") return MessageType::Sticker;
    if (msgtype.find("m.key.verification") == 0) return MessageType::KeyVerification;

    return MessageType::Unknown;
}

MessageContent parseMessageContent(const std::string& contentJson) {
    MessageContent content;
    content.type = parseMessageType(contentJson);

    // Helper: extract string field
    auto extractStr = [&](const std::string& field) -> std::string {
        auto search = "\"" + field + "\":\"";
        auto pos = contentJson.find(search);
        if (pos == std::string::npos) {
            search = "\"" + field + "\": \"";
            pos = contentJson.find(search);
        }
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = contentJson.find('"', pos);
        if (end == std::string::npos) return "";
        return contentJson.substr(pos, end - pos);
    };

    // Helper: extract int field
    auto extractInt = [&](const std::string& field) -> int {
        auto search = "\"" + field + "\":";
        auto pos = contentJson.find(search);
        if (pos == std::string::npos) return 0;
        pos += search.size();
        while (pos < contentJson.size() && (contentJson[pos] == ' ' || contentJson[pos] == '\t')) pos++;
        int val = 0;
        while (pos < contentJson.size() && contentJson[pos] >= '0' && contentJson[pos] <= '9') {
            val = val * 10 + (contentJson[pos] - '0');
            pos++;
        }
        return val;
    };

    content.body = extractStr("body");
    content.format = extractStr("format");
    content.formattedBody = extractStr("formatted_body");
    content.filename = extractStr("filename");
    content.mimetype = extractStr("mimetype");
    content.geoUri = extractStr("geo_uri");

    // Extract URL/media info from nested info/url fields
    content.mxcUrl = extractStr("url");

    // Extract thumbnail info
    if (contentJson.find("\"thumbnail_url\"") != std::string::npos) {
        content.hasThumbnail = true;
        content.thumbnailUrl = extractStr("thumbnail_url");
    }

    // Extract dimensions (info.w, info.h) from media events
    if (content.type == MessageType::Image || content.type == MessageType::Video) {
        content.imageWidth = extractInt("w");
        content.imageHeight = extractInt("h");
        if (contentJson.find("\"thumbnail_info\"") != std::string::npos) {
            content.hasThumbnail = true;
        }
    }

    // Size
    content.size = static_cast<int64_t>(extractInt("size"));
    content.durationMs = extractInt("duration");

    // Check for edit (m.relates_to → m.replace)
    if (contentJson.find("\"rel_type\":\"m.replace\"") != std::string::npos ||
        contentJson.find("\"rel_type\": \"m.replace\"") != std::string::npos) {
        content.isEdit = true;
    }

    // Check for reply (m.relates_to → m.in_reply_to)
    if (contentJson.find("\"m.in_reply_to\"") != std::string::npos) {
        content.isReply = true;
    }

    return content;
}

bool supportsThumbnails(MessageType type) {
    return type == MessageType::Image || type == MessageType::Video ||
           type == MessageType::Sticker || type == MessageType::File;
}

bool isInlineDisplayable(MessageType type) {
    return type == MessageType::Image || type == MessageType::Sticker;
}

bool isMediaType(MessageType type) {
    return type == MessageType::Image || type == MessageType::Video ||
           type == MessageType::Audio || type == MessageType::File ||
           type == MessageType::Sticker || type == MessageType::Location;
}

std::string getMessageTypeLabel(MessageType type) {
    switch (type) {
        case MessageType::Text: return "Text Message";
        case MessageType::Notice: return "Notice";
        case MessageType::Emote: return "Emote";
        case MessageType::Image: return "Image";
        case MessageType::Video: return "Video";
        case MessageType::Audio: return "Audio";
        case MessageType::File: return "File";
        case MessageType::Location: return "Location";
        case MessageType::Sticker: return "Sticker";
        case MessageType::KeyVerification: return "Key Verification";
        default: return "Unknown";
    }
}

std::string getExtensionFromMimeType(const std::string& mimetype) {
    if (mimetype == "image/jpeg") return ".jpg";
    if (mimetype == "image/png") return ".png";
    if (mimetype == "image/gif") return ".gif";
    if (mimetype == "image/webp") return ".webp";
    if (mimetype == "image/svg+xml") return ".svg";
    if (mimetype == "video/mp4") return ".mp4";
    if (mimetype == "video/webm") return ".webm";
    if (mimetype == "audio/mpeg") return ".mp3";
    if (mimetype == "audio/ogg") return ".ogg";
    if (mimetype == "audio/wav") return ".wav";
    if (mimetype == "application/pdf") return ".pdf";
    if (mimetype == "application/zip") return ".zip";
    if (mimetype == "text/plain") return ".txt";
    return "";
}

std::string formatFileSize(int64_t bytes) {
    if (bytes <= 0) return "0 B";

    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << size << " " << units[unitIndex];
    return out.str();
}

std::string messageContentToJson(const MessageContent& content) {
    auto esc = [](const std::string& s) -> std::string {
        return escapeJson(s);
    };
    std::ostringstream json;
    json << "{";
    json << R"("type": )" << static_cast<int>(content.type) << ",";
    json << R"("typeLabel": ")" << esc(getMessageTypeLabel(content.type)) << R"(",)";
    json << R"("body": ")" << esc(content.body) << R"(",)";
    json << R"("format": ")" << esc(content.format) << R"(",)";
    json << R"("mxcUrl": ")" << esc(content.mxcUrl) << R"(",)";
    json << R"("mimetype": ")" << esc(content.mimetype) << R"(",)";
    json << R"("filename": ")" << esc(content.filename) << R"(",)";
    json << R"("size": )" << content.size << ",";
    json << R"("imageWidth": )" << content.imageWidth << ",";
    json << R"("imageHeight": )" << content.imageHeight << ",";
    json << R"("durationMs": )" << content.durationMs << ",";
    json << R"("hasThumbnail": )" << (content.hasThumbnail ? "true" : "false") << ",";
    json << R"("isMedia": )" << (isMediaType(content.type) ? "true" : "false") << ",";
    json << R"("isEdit": )" << (content.isEdit ? "true" : "false");
    json << "}";
    return json.str();
}

// ==== Reply Text Extraction (from ContentUtils.kt:23-50) ====
// Original Kotlin:
//   fun extractUsefulTextFromReply(repliedBody: String): String {
//       val lines = repliedBody.lines()
//       var wellFormed = repliedBody.startsWith(">")
//       var endOfPreviousFound = false
//       val usefulLines = ArrayList<String>()
//       lines.forEach {
//           if (it == "") { endOfPreviousFound = true; return@forEach }
//           if (!endOfPreviousFound) {
//               wellFormed = wellFormed && it.startsWith(">")
//           } else { usefulLines.add(it) }
//       }
//       return usefulLines.joinToString("\n").takeIf { wellFormed } ?: repliedBody
//   }

std::string extractUsefulTextFromReply(const std::string& repliedBody) {
    if (repliedBody.empty()) return "";

    // Split into lines
    std::vector<std::string> lines;
    std::string current;
    for (char c : repliedBody) {
        if (c == '\n') { lines.push_back(current); current.clear(); }
        else current += c;
    }
    if (!current.empty()) lines.push_back(current);

    // Original: wellFormed = repliedBody.startsWith(">")
    bool wellFormed = !lines.empty() && !lines[0].empty() && lines[0][0] == '>';
    bool endOfPreviousFound = false;
    std::string usefulText;

    for (const auto& line : lines) {
        // Original: if (it == "") { endOfPreviousFound = true; return@forEach }
        if (line.empty()) {
            endOfPreviousFound = true;
            continue;
        }
        if (!endOfPreviousFound) {
            // Original: wellFormed = wellFormed && it.startsWith(">")
            wellFormed = wellFormed && !line.empty() && line[0] == '>';
        } else {
            if (!usefulText.empty()) usefulText += '\n';
            usefulText += line;
        }
    }

    // Original: return usefulLines.joinToString("\n").takeIf { wellFormed } ?: repliedBody
    return wellFormed ? usefulText : repliedBody;
}

std::string extractUsefulTextFromHtmlReply(
    const std::string& repliedHtmlBody,
    const std::string& mxReplyStartTag,
    const std::string& mxReplyEndTag)
{
    // Original: if (repliedBody.startsWith(MX_REPLY_START_TAG))
    if (repliedHtmlBody.find(mxReplyStartTag) != 0) return repliedHtmlBody;

    auto closingTagIndex = repliedHtmlBody.rfind(mxReplyEndTag);
    if (closingTagIndex == std::string::npos) return repliedHtmlBody;

    std::string result = repliedHtmlBody.substr(closingTagIndex + mxReplyEndTag.size());
    // Trim leading whitespace
    while (!result.empty() && result[0] == ' ') result.erase(0, 1);
    while (!result.empty() && result[0] == '\n') result.erase(0, 1);
    return result;
}

std::string formatSpoilerTextFromHtml(const std::string& formattedBody) {
    // Original: replaces <span data-mx-spoiler>content</span> with spoiler chars
    // Replaces content between <span data-mx-spoiler> and </span> with block chars
    std::string result;
    const std::string SPOILER_OPEN = "<span data-mx-spoiler>";
    const std::string SPOILER_CLOSE = "</span>";
    const char SPOILER_CHAR = '\xDB'; // █

    size_t pos = 0;
    while (pos < formattedBody.size()) {
        auto openPos = formattedBody.find(SPOILER_OPEN, pos);
        if (openPos == std::string::npos) {
            result += formattedBody.substr(pos);
            break;
        }

        result += formattedBody.substr(pos, openPos - pos);
        openPos += SPOILER_OPEN.size();

        auto closePos = formattedBody.find(SPOILER_CLOSE, openPos);
        if (closePos == std::string::npos) {
            result += formattedBody.substr(openPos - SPOILER_OPEN.size());
            pos = openPos;
            continue;
        }

        // Count visible characters between tags
        std::string content = formattedBody.substr(openPos, closePos - openPos);
        size_t visibleChars = 0;
        bool inTag = false;
        for (char c : content) { if (c == '<') inTag = true; else if (c == '>') inTag = false; else if (!inTag) visibleChars++; }

        // Replace with spoiler chars
        result += std::string(visibleChars, SPOILER_CHAR);
        pos = closePos + SPOILER_CLOSE.size();
    }

    return result;
}

// ==== Combined Text + Image (Element X-style messages) ====
// Matrix spec: m.room.message with msgtype=m.text, formatted_body contains <img>

std::string buildTextWithImageContent(
    const std::string& plainText, const std::string& imageMxcUrl,
    const std::string& imageMimetype, int imageWidth, int imageHeight)
{
    auto esc = [](const std::string& s) -> std::string {
        std::string out; for (char c : s) { if (c == '"') out += "\\\""; if (c == '<') out += "&lt;"; else out += c; } return out;
    };

    std::ostringstream json;
    json << R"({"msgtype": "m.text",)";
    json << R"("body": ")" << esc(plainText) << R"(",)";
    json << R"("format": "org.matrix.custom.html",)";
    json << R"("formatted_body": ")" << esc(plainText);

    // Add inline image as HTML <img> tag
    json << "<br><img src=\\\"" << esc(imageMxcUrl) << "\\\"";
    if (!imageMimetype.empty()) json << " data-mime-type=\\\"" << esc(imageMimetype) << "\\\"";
    if (imageWidth > 0) json << " width=\\\"" << imageWidth << "\\\"";
    if (imageHeight > 0) json << " height=\\\"" << imageHeight << "\\\"";
    json << ">";

    json << R"(")";
    json << "}";
    return json.str();
}

std::string buildReplyWithImageContent(
    const std::string& plainText, const std::string& imageMxcUrl,
    const std::string& replyEventId, const std::string& imageMimetype)
{
    auto esc = [](const std::string& s) -> std::string {
        return escapeJson(s);
    };

    std::ostringstream json;
    json << R"({"msgtype": "m.text",)";
    json << R"("body": ")" << esc(plainText) << R"(",)";
    json << R"("format": "org.matrix.custom.html",)";
    json << R"("formatted_body": ")" << esc(plainText) << "<br><img src='" << esc(imageMxcUrl) << "'/>" << R"(",)";
    json << R"("m.relates_to": {)";
    json << R"("m.in_reply_to": {)";
    json << R"("event_id": ")" << esc(replyEventId) << R"(")";
    json << "}}}";
    return json.str();
}

bool hasTextWithImage(const std::string& contentJson) {
    return contentJson.find("\"format\"") != std::string::npos &&
           contentJson.find("\"formatted_body\"") != std::string::npos &&
           contentJson.find("<img") != std::string::npos;
}

// ==== MIME Type Detection (from MimeTypes.kt:39-48) ====
std::string normalizeMimeType(const std::string& mimeType) {
    // "image/jpg" → "image/jpeg"
    if (mimeType == "image/jpg") return "image/jpeg";
    return mimeType;
}

bool isMimeTypeImage(const std::string& mt) { return mt.find("image/") == 0; }
bool isMimeTypeVideo(const std::string& mt) { return mt.find("video/") == 0; }
bool isMimeTypeAudio(const std::string& mt) { return mt.find("audio/") == 0; }
bool isMimeTypeText(const std::string& mt) { return mt.find("text/") == 0; }

// ==== Timeline Event Content (from TimelineEvent.kt:121-233) ====

std::string getLatestEditEventId(const std::string& editSummaryJson, const std::string& originalEventId) {
    // Original: annotations?.editSummary?.sourceEvents?.lastOrNull() ?: eventId
    if (editSummaryJson.empty()) return originalEventId;

    // Find last event_id in sourceEvents array
    auto eventsPos = editSummaryJson.find("\"sourceEvents\"");
    if (eventsPos == std::string::npos) return originalEventId;

    // Find last quoted event_id
    auto lastQuote = editSummaryJson.rfind("\"event_id\":\"", editSummaryJson.find(']', eventsPos));
    if (lastQuote == std::string::npos) {
        lastQuote = editSummaryJson.rfind("\"event_id\": \"", editSummaryJson.find(']', eventsPos));
    }
    if (lastQuote == std::string::npos || lastQuote < eventsPos) return originalEventId;

    lastQuote += 12; // skip "event_id":"
    auto end = editSummaryJson.find('"', lastQuote);
    return (end != std::string::npos) ? editSummaryJson.substr(lastQuote, end - lastQuote) : originalEventId;
}

std::string getEditedTargetEventId(const std::string& contentJson) {
    // Original: getRelationContent()?.takeIf { it.type == REPLACE }?.eventId
    auto relatesPos = contentJson.find("\"m.relates_to\"");
    if (relatesPos == std::string::npos) return "";

    // Find rel_type: "m.replace"
    auto typePos = contentJson.find("\"rel_type\":\"m.replace\"", relatesPos);
    if (typePos == std::string::npos) {
        typePos = contentJson.find("\"rel_type\": \"m.replace\"", relatesPos);
    }
    if (typePos == std::string::npos || typePos > contentJson.find('}', relatesPos)) return "";

    // Find event_id
    auto eventPos = contentJson.find("\"event_id\":\"", relatesPos);
    if (eventPos == std::string::npos) {
        eventPos = contentJson.find("\"event_id\": \"", relatesPos);
    }
    if (eventPos == std::string::npos || eventPos > contentJson.find('}', relatesPos)) return "";

    eventPos += 12;
    auto end = contentJson.find('"', eventPos);
    return (end != std::string::npos) ? contentJson.substr(eventPos, end - eventPos) : "";
}

std::string getTextEditableContent(const std::string& contentJson,
    const std::string& editSummaryJson, bool formatted)
{
    // Original: getLastMessageContent() then get body or formattedBody
    // First, try to get the latest edited body
    std::string body;
    auto extractStr = [](const std::string& json, const std::string& key) -> std::string {
        auto search = "\"" + key + "\":\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) {
            search = "\"" + key + "\": \"";
            pos = json.find(search);
        }
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = json.find('"', pos);
        return (end != std::string::npos) ? json.substr(pos, end - pos) : "";
    };

    // Check if it's a reply — strip quoted lines if so
    bool isReply = contentJson.find("\"m.in_reply_to\"") != std::string::npos;

    if (formatted) {
        body = extractStr(contentJson, "formatted_body");
        if (body.empty()) body = extractStr(contentJson, "body");
    } else {
        body = extractStr(contentJson, "body");
    }

    if (isReply && !body.empty()) {
        body = extractUsefulTextFromReply(body);
    }

    return body;
}

bool isReplyEvent(const std::string& contentJson) {
    return contentJson.find("\"m.in_reply_to\"") != std::string::npos;
}

bool isEditionEvent(const std::string& contentJson) {
    return !getEditedTargetEventId(contentJson).empty();
}

std::string ensureCorrectFormattedBodyInTextReply(
    const std::string& newFormattedBody,
    const std::string& newBody,
    const std::string& originalFormattedBody) {

    const char* MX_REPLY_END_TAG = "</mx-reply>";

    // Only fix if new formatted_body is missing the reply end tag
    // while the original had one
    if (newFormattedBody.empty()) return newBody;
    if (newFormattedBody.find(MX_REPLY_END_TAG) != std::string::npos) return newFormattedBody;
    if (originalFormattedBody.find(MX_REPLY_END_TAG) == std::string::npos) return newFormattedBody;

    // Merge: take original's <mx-reply>... wrapper + new body
    auto endPos = originalFormattedBody.rfind(MX_REPLY_END_TAG);
    if (endPos == std::string::npos) return newFormattedBody;
    endPos += strlen(MX_REPLY_END_TAG);

    return originalFormattedBody.substr(0, endPos) + newBody;
}

} // namespace progressive

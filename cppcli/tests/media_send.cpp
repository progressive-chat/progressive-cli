// src/media_send.cpp — see media_send.hpp.
#include "media_send.hpp"

#include <cstdio>
#include <cstdint>

namespace matrixcli {
namespace media {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Appends the m.relates_to (thread) relation before the final '}'.
void appendThreadRel(std::string& json, const std::string& threadRoot) {
    if (threadRoot.empty()) return;
    size_t pos = json.rfind('}');
    if (pos != std::string::npos) {
        json.insert(pos, ",\"m.relates_to\":{\"rel_type\":\"m.thread\",\"event_id\":\""
                             + jsonEscape(threadRoot) + "\"}");
    }
}

void appendInfo(std::string& json, const std::string& contentType, size_t sizeBytes,
                int imgW, int imgH, bool hasImgDim, bool full) {
    json += ",\"info\":{\"mimetype\":\"" + contentType + "\"";
    if (full) {
        json += ",\"size\":" + std::to_string(sizeBytes);
        if (hasImgDim) {
            json += ",\"w\":" + std::to_string(imgW) + ",\"h\":" + std::to_string(imgH);
        }
    }
    json += "}";
}

} // namespace

bool imageDimensions(const std::vector<uint8_t>& b, const std::string& ext,
                     int& w, int& h) {
    auto be16 = [&b](size_t o) { return b.size() > o + 1 ? (b[o] << 8) | b[o + 1] : 0; };
    auto be32 = [&b](size_t o) {
        return b.size() > o + 3 ? (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3] : 0;
    };
    if (ext == "png" && b.size() >= 24 && b[0] == 0x89 && b[1] == 'P') {
        w = static_cast<int>(be32(16));
        h = static_cast<int>(be32(20));
        return w > 0 && h > 0;
    }
    if (ext == "gif" && b.size() >= 10 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F') {
        w = be16(6);
        h = be16(8);
        return w > 0 && h > 0;
    }
    if ((ext == "jpg" || ext == "jpeg") && b.size() >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        // Walk the JPEG segments to the first SOF marker.
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (b[i] != 0xFF) { i++; continue; }
            uint8_t marker = b[i + 1];
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                marker != 0xCC) {
                h = static_cast<int>(be16(i + 5));
                w = static_cast<int>(be16(i + 7));
                return w > 0 && h > 0;
            }
            if (marker == 0xD8 || marker == 0xD9) { i += 2; continue; }
            if (marker == 0xDA || marker == 0x01) break;  // no more SOFs after the scan
            size_t len = be16(i + 2);
            if (len < 2) break;
            i += 2 + len;
        }
    }
    return false;
}

std::string plainContent(const std::string& msgtype, const std::string& body,
                         const std::string& filename, const std::string& mxc,
                         const std::string& contentType, size_t sizeBytes,
                         int imgW, int imgH, bool hasImgDim,
                         const std::string& preset, const std::string& threadRoot) {
    std::string json = "{\"msgtype\":\"" + msgtype + "\",\"body\":\"" + jsonEscape(body) + "\"";
    if (preset != "compact") {
        json += ",\"filename\":\"" + jsonEscape(filename) + "\",\"url\":\"" + mxc + "\"";
    }
    if (preset == "full") {
        appendInfo(json, contentType, sizeBytes, imgW, imgH, hasImgDim, true);
    }
    json += "}";
    appendThreadRel(json, threadRoot);
    return json;
}

std::string encryptedContent(const std::string& msgtype, const std::string& body,
                             const std::string& filename, const std::string& url,
                             const std::string& key, const std::string& iv,
                             const std::string& sha256B64,
                             const std::string& contentType, size_t sizeBytes,
                             int imgW, int imgH, bool hasImgDim,
                             const std::string& preset, const std::string& threadRoot) {
    std::string json = "{\"msgtype\":\"" + msgtype + "\",\"body\":\"" + jsonEscape(body) + "\"";
    if (preset != "compact") {
        json += ",\"filename\":\"" + jsonEscape(filename) + "\"";
    }
    json += ",\"file\":{\"url\":\"" + url + "\",\"key\":\"" + key + "\",\"iv\":\"" + iv +
            "\",\"hashes\":{\"sha256\":\"" + sha256B64 + "\"},\"v\":\"v2\",\"mimetype\":\"" +
            contentType + "\"}";
    if (preset == "full") {
        appendInfo(json, contentType, sizeBytes, imgW, imgH, hasImgDim, true);
    } else if (preset == "original") {
        appendInfo(json, contentType, sizeBytes, imgW, imgH, hasImgDim, false);
    }
    json += "}";
    appendThreadRel(json, threadRoot);
    return json;
}

} // namespace media
} // namespace matrixcli

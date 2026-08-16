// src/media_send.hpp — the media send content builders (the sendpreset
// shapes: original/compact/full) + the header-level image dimensions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace matrixcli {
namespace media {

// The image size straight from the file header (PNG/JPEG/GIF), no
// libraries. Returns false when the format is not one of the three.
bool imageDimensions(const std::vector<uint8_t>& b, const std::string& ext,
                     int& w, int& h);

// The unencrypted m.room.message content JSON:
//   original — msgtype + body + filename + url
//   compact  — msgtype + body only
//   full     — plus the info block (mimetype, size, w/h when an image)
std::string plainContent(const std::string& msgtype, const std::string& body,
                         const std::string& filename, const std::string& mxc,
                         const std::string& contentType, size_t sizeBytes,
                         int imgW, int imgH, bool hasImgDim,
                         const std::string& preset, const std::string& threadRoot);

// The encrypted content JSON: msgtype + body (+ filename unless compact)
// + the AES-CTR file block (+ the info: compact drops it, original keeps
// the mimetype, full adds the size and the dimensions).
std::string encryptedContent(const std::string& msgtype, const std::string& body,
                             const std::string& filename, const std::string& url,
                             const std::string& key, const std::string& iv,
                             const std::string& sha256B64,
                             const std::string& contentType, size_t sizeBytes,
                             int imgW, int imgH, bool hasImgDim,
                             const std::string& preset, const std::string& threadRoot);

} // namespace media
} // namespace matrixcli

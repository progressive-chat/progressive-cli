// test_media_send.cpp — the send-preset content builders + the header
// image dimensions. The JSON shapes must match what the clients expect.
#include "../src/media_send.hpp"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using matrixcli::media::encryptedContent;
using matrixcli::media::imageDimensions;
using matrixcli::media::plainContent;

static int failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "FAIL " << __LINE__ << ": " << #cond << std::endl;   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

// A synthetic PNG header (89 50 4E 47 + IHDR) with width/height set.
static std::vector<uint8_t> fakePng(int w, int h) {
    std::vector<uint8_t> b(24, 0);
    b[0] = 0x89; b[1] = 'P'; b[2] = 'N'; b[3] = 'G';
    b[4] = '\r'; b[5] = '\n'; b[6] = 0x1A; b[7] = '\n';
    b[12] = 'I'; b[13] = 'H'; b[14] = 'D'; b[15] = 'R';
    b[16] = (w >> 24) & 0xFF; b[17] = (w >> 16) & 0xFF;
    b[18] = (w >> 8) & 0xFF; b[19] = w & 0xFF;
    b[20] = (h >> 24) & 0xFF; b[21] = (h >> 16) & 0xFF;
    b[22] = (h >> 8) & 0xFF; b[23] = h & 0xFF;
    return b;
}

int main() {
    // --- imageDimensions ---
    {
        int w = 0, h = 0;
        CHECK(imageDimensions(fakePng(1920, 1080), "png", w, h));
        CHECK(w == 1920 && h == 1080);
        CHECK(!imageDimensions(fakePng(1920, 1080), "jpg", w, h));  // wrong ext
        std::vector<uint8_t> junk = {'h', 'e', 'l', 'l', 'o'};
        CHECK(!imageDimensions(junk, "png", w, h));
    }

    // --- plainContent: original ---
    {
        std::string j = plainContent("m.file", "a.txt", "a.txt", "mxc://hs/x",
                                     "text/plain", 42, 0, 0, false, "original", "");
        auto o = nlohmann::json::parse(j);
        CHECK(o["msgtype"] == "m.file");
        CHECK(o["body"] == "a.txt");
        CHECK(o["filename"] == "a.txt");
        CHECK(o["url"] == "mxc://hs/x");
        CHECK(!o.contains("info"));
    }
    // --- plainContent: compact drops everything optional ---
    {
        std::string j = plainContent("m.file", "a.txt", "a.txt", "mxc://hs/x",
                                     "text/plain", 42, 0, 0, false, "compact", "");
        auto o = nlohmann::json::parse(j);
        CHECK(o["msgtype"] == "m.file");
        CHECK(o["body"] == "a.txt");
        CHECK(!o.contains("filename"));
        CHECK(!o.contains("url"));
        CHECK(!o.contains("info"));
    }
    // --- plainContent: full adds the info with the size + dimensions ---
    {
        std::string j = plainContent("m.image", "p.png", "p.png", "mxc://hs/p",
                                     "image/png", 1234, 640, 480, true, "full", "");
        auto o = nlohmann::json::parse(j);
        CHECK(o["info"]["mimetype"] == "image/png");
        CHECK(o["info"]["size"] == 1234);
        CHECK(o["info"]["w"] == 640 && o["info"]["h"] == 480);
    }
    // --- plainContent: the thread relation lands before the close ---
    {
        std::string j = plainContent("m.text", "hi", "", "", "text/plain", 0, 0, 0,
                                     false, "compact", "$evt:hs");
        auto o = nlohmann::json::parse(j);
        CHECK(o["m.relates_to"]["rel_type"] == "m.thread");
        CHECK(o["m.relates_to"]["event_id"] == "$evt:hs");
    }

    // --- encryptedContent: compact keeps the file block, drops the rest ---
    {
        std::string j = encryptedContent("m.file", "a.bin", "a.bin", "mxc://hs/e",
                                         "K", "I", "S", "application/octet-stream",
                                         9, 0, 0, false, "compact", "");
        auto o = nlohmann::json::parse(j);
        CHECK(o["file"]["url"] == "mxc://hs/e");
        CHECK(o["file"]["key"] == "K");
        CHECK(o["file"]["iv"] == "I");
        CHECK(o["file"]["hashes"]["sha256"] == "S");
        CHECK(o["file"]["v"] == "v2");
        CHECK(!o.contains("filename"));
        CHECK(!o.contains("info"));
    }
    // --- encryptedContent: original keeps filename + the bare info ---
    {
        std::string j = encryptedContent("m.file", "a.bin", "a.bin", "mxc://hs/e",
                                         "K", "I", "S", "application/octet-stream",
                                         9, 0, 0, false, "original", "");
        auto o = nlohmann::json::parse(j);
        CHECK(o["filename"] == "a.bin");
        CHECK(o["info"]["mimetype"] == "application/octet-stream");
        CHECK(!o["info"].contains("size"));
    }
    // --- encryptedContent: full carries the size + the dimensions ---
    {
        std::string j = encryptedContent("m.image", "p.png", "p.png", "mxc://hs/e",
                                         "K", "I", "S", "image/png",
                                         2048, 320, 200, true, "full", "");
        auto o = nlohmann::json::parse(j);
        CHECK(o["info"]["size"] == 2048);
        CHECK(o["info"]["w"] == 320 && o["info"]["h"] == 200);
    }
    // --- the escaping: quotes and control chars in the caption ---
    {
        std::string j = plainContent("m.text", "say \"hi\"\nnow", "", "",
                                     "text/plain", 0, 0, 0, false, "compact", "");
        auto o = nlohmann::json::parse(j);
        CHECK(o["body"] == "say \"hi\"\nnow");
    }

    if (failures) {
        std::cerr << failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "media_send: all checks passed" << std::endl;
    return 0;
}

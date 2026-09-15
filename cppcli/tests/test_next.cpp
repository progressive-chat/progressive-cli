// test_next.cpp — regression tests for the "next" branch work.
//
// Covers the room-identifier URL encoding behind join/knock/leave/invite
// (room aliases start with '#', a URL fragment delimiter, so they must be
// percent-encoded before being spliced into request paths) plus the
// ProxyConfig validation used by the proxy/ttys plumbing.
#include "../lib/http/http.hpp"

#include <iostream>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "FAIL " << __LINE__ << ": " << #cond << std::endl;   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

using matrixcli::http::ProxyConfig;
using matrixcli::http::ProxyType;
using matrixcli::http::urlEncode;

int main() {
    // The join fix: '#' must not survive into the request path.
    CHECK(urlEncode("#general:matrix.org") == "%23general%3Amatrix.org");
    CHECK(urlEncode("#room:server") == "%23room%3Aserver");

    // Room IDs encode too ('!' and ':' are reserved).
    CHECK(urlEncode("!roomid:server") == "%21roomid%3Aserver");

    // Unreserved characters pass through untouched (RFC 3986).
    CHECK(urlEncode("abcXYZ019-_.~") == "abcXYZ019-_.~");

    // Common reserved characters.
    CHECK(urlEncode("hello world") == "hello%20world");
    CHECK(urlEncode("a/b?c=d&e") == "a%2Fb%3Fc%3Dd%26e");

    // Empty input stays empty.
    CHECK(urlEncode("").empty());

    // Non-ASCII bytes become uppercase %XX per byte (UTF-8 e-acute).
    CHECK(urlEncode("\xC3\xA9") == "%C3%A9");

    // ProxyConfig::enabled(): host + port + type required.
    {
        ProxyConfig p;
        CHECK(!p.enabled());
        p.type = ProxyType::SOCKS5;
        p.host = "127.0.0.1";
        p.port = 9050;
        CHECK(p.enabled());
        p.port = 0;
        CHECK(!p.enabled());
        p.port = 9050;
        p.host.clear();
        CHECK(!p.enabled());
    }

    if (failures == 0) std::cout << "test_next: all checks passed" << std::endl;
    return failures == 0 ? 0 : 1;
}

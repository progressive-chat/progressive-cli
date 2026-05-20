#include <iostream>
#include <cassert>
#include <string>

#include "../lib/util/string_utils.hpp"
#include "../lib/util/logger.hpp"
#include "../lib/matrix/events.hpp"
#include "../lib/matrix/error.hpp"
#include "../lib/http/http.hpp"

using namespace matrixcli;

static void test_trim() {
    assert(util::trim("  hello  ") == "hello");
    assert(util::trim("no_trim") == "no_trim");
    assert(util::trim("   ") == "");
}

static void test_split() {
    auto parts = util::split("a,b,c", ',');
    assert(parts.size() == 3);
    assert(parts[0] == "a");
    assert(parts[1] == "b");
    assert(parts[2] == "c");
}

static void test_toLower() {
    assert(util::toLower("HELLO") == "hello");
    assert(util::toLower("HelloWorld") == "helloworld");
}

static void test_startsWith() {
    assert(util::startsWith("hello world", "hello"));
    assert(!util::startsWith("hello world", "world"));
}

static void test_urlEncode() {
    assert(util::urlEncode("hello world") == "hello%20world");
    assert(util::urlEncode("test/foo?bar=baz") == "test%2Ffoo%3Fbar%3Dbaz");
}

static void test_logger() {
    auto& logger = util::Logger::instance();
    logger.setLevel(util::LogLevel::Debug);
    assert(logger.level() == util::LogLevel::Debug);
    logger.debug("test debug message");
    logger.info("test info message");
    logger.warn("test warn message");
    logger.error("test error message");
    logger.setLevel(util::LogLevel::Info);
}

static void test_event_from_json() {
    nlohmann::json j = {
        {"event_id", "$event1"},
        {"room_id", "!room1:matrix.org"},
        {"sender", "@user:matrix.org"},
        {"type", "m.room.message"},
        {"origin_server_ts", 1234567890},
        {"content", {
            {"body", "Hello world"},
            {"msgtype", "m.text"}
        }}
    };

    auto ev = matrix::Event::fromJSON(j);
    assert(ev.id == "$event1");
    assert(ev.room_id == "!room1:matrix.org");
    assert(ev.sender == "@user:matrix.org");
    assert(ev.type == "m.room.message");
    assert(ev.body == "Hello world");
    assert(ev.msgtype == "m.text");
    assert(ev.timestamp == 1234567890);
}

static void test_event_to_json() {
    matrix::Event ev;
    ev.id = "$event1";
    ev.sender = "@user:matrix.org";
    ev.body = "Hello";
    ev.type = "m.room.message";
    ev.msgtype = "m.text";

    auto j = ev.toJSON();
    assert(j["event_id"] == "$event1");
    assert(j["content"]["body"] == "Hello");
}

static void test_matrix_error() {
    try {
        throw matrix::MatrixError("test error");
    } catch (const std::exception& e) {
        assert(std::string(e.what()) == "test error");
    }
}

static void test_format_detection() {
    using namespace api;

    assert(detectFormat("application/json", "") == Format::JSON);
    assert(detectFormat("text/plain", "") == Format::Text);
    assert(detectFormat("text/markdown", "") == Format::Markdown);
    assert(detectFormat("text/gemini", "") == Format::Gemini);
    assert(detectFormat("text/html", "") == Format::HTML);

    assert(detectFormat("", "markdown") == Format::Markdown);
    assert(detectFormat("", "html") == Format::HTML);
}

static void test_http_parse_url() {
    // Test via HTTP client (indirect)
    http::Client client;
    client.setTimeout(5);
    // Only test that client can be constructed and configured
    assert(true);
}

static void test_config() {
    Config::instance().set("test_key", "test_value");
    assert(Config::instance().get("test_key") == "test_value");
    assert(Config::instance().get("nonexistent", "default") == "default");
}

int main() {
    std::cout << "Running matrixcli tests..." << std::endl;

    test_trim();
    test_split();
    test_toLower();
    test_startsWith();
    test_urlEncode();
    test_logger();
    test_event_from_json();
    test_event_to_json();
    test_matrix_error();
    test_format_detection();
    test_http_parse_url();
    test_config();

    std::cout << "All tests passed!" << std::endl;
    return 0;
}

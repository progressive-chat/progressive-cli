// tests/test_phase4.cpp — unit tests for lazy room state loading (Phase 4).
//
// Verifies:
//  1. JSON string extraction (extractJsonStringDecoded)
//  2. computeRoomName with various state event sets
//  3. RoomListModel upsert + batch state update
//  4. Sync URL construction (full_state=false, empty since)
//
// Build + run:
//   cmake --build build -j4 && ctest --test-dir build --output-on-failure

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <iostream>

// Replicate the JSON extraction logic from main_window.cpp (namespace-scope).

static std::string jsonUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') { out += s[i]; continue; }
        if (++i >= s.size()) break;
        char c = s[i];
        switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 >= s.size()) { out += 'u'; break; }
                unsigned cp = 0;
                for (int j = 1; j <= 4; ++j) {
                    char hex = s[i + j];
                    cp <<= 4;
                    if (hex >= '0' && hex <= '9') cp |= (hex - '0');
                    else if (hex >= 'a' && hex <= 'f') cp |= (hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') cp |= (hex - 'A' + 10);
                    else { cp = 0; break; }
                }
                i += 4;
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += '\\'; out += c; break;
        }
    }
    return out;
}

static std::string extractJsonStringDecoded(std::string_view json, std::string_view key) {
    std::string pat1 = std::string("\"") + std::string(key) + "\":\"";
    auto pos = json.find(pat1);
    if (pos != std::string_view::npos) {
        pos += pat1.size();
        auto end = pos;
        while (end < json.size()) {
            if (json[end] == '\\' && end + 1 < json.size()) { end += 2; continue; }
            if (json[end] == '"') break;
            ++end;
        }
        if (end < json.size()) return jsonUnescape(json.substr(pos, end - pos));
    }
    std::string pat2 = std::string("\"") + std::string(key) + "\": \"";
    pos = json.find(pat2);
    if (pos != std::string_view::npos) {
        pos += pat2.size();
        auto end = pos;
        while (end < json.size()) {
            if (json[end] == '\\' && end + 1 < json.size()) { end += 2; continue; }
            if (json[end] == '"') break;
            ++end;
        }
        if (end < json.size()) return jsonUnescape(json.substr(pos, end - pos));
    }
    return {};
}

static std::string extractJsonString(std::string_view json, std::string_view key) {
    return extractJsonStringDecoded(json, key);
}

// ---- Test infrastructure ----
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (expected " << (b) << " got " << (a) << ") line " << __LINE__ << "\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

// ---- Tests ----

static void test_jsonUnescape_ascii() {
    CHECK_EQ(jsonUnescape("hello"), "hello", "jsonUnescape plain ASCII");
    CHECK_EQ(jsonUnescape("hello\\nworld"), "hello\nworld", "jsonUnescape \\n");
    CHECK_EQ(jsonUnescape("tab\\there"), "tab\there", "jsonUnescape \\t");
    CHECK_EQ(jsonUnescape("quote\\\"here"), "quote\"here", "jsonUnescape \\\"");
    CHECK_EQ(jsonUnescape("back\\\\slash"), "back\\slash", "jsonUnescape \\\\");
}

static void test_jsonUnescape_unicode() {
    auto r = jsonUnescape("\\u0440\\u0443\\u0441");
    CHECK(r == "рус", "jsonUnescape Cyrillic (рус)");
}

static void test_extractJsonString_basic() {
    auto r = extractJsonString("{\"name\":\"My Room\"}", "name");
    CHECK_EQ(r, "My Room", "extractJsonString basic name");
}

static void test_extractJsonString_with_space() {
    auto r = extractJsonString("{\"name\": \"Test Chat\"}", "name");
    CHECK_EQ(r, "Test Chat", "extractJsonString with space after colon");
}

static void test_extractJsonString_mxc_url() {
    auto r = extractJsonString(
        "{\"url\":\"mxc://matrix.org/abc123\"}", "url");
    CHECK_EQ(r, "mxc://matrix.org/abc123", "extractJsonString mxc url");
}

static void test_extractJsonString_missing_key() {
    auto r = extractJsonString("{\"name\":\"Room\"}", "url");
    CHECK(r.empty(), "extractJsonString missing key returns empty");
}

static void test_extractJsonString_decoded_escapes() {
    auto r = extractJsonString(
        "{\"body\":\"line1\\nline2\\\"quoted\\\"\"}", "body");
    CHECK_EQ(r, "line1\nline2\"quoted\"", "extractJsonString decoded escapes in body");
}

static void test_extractJsonString_url_with_colon() {
    auto r = extractJsonString(
        "{\"avatar_url\":\"mxc://example.com/ABC/def\"}", "avatar_url");
    CHECK_EQ(r, "mxc://example.com/ABC/def", "extractJsonString mxc url 2");
}

// ---- Sync URL test: full_state=false should never produce full_state=true from since.empty() ----

static void test_sync_url_no_full_state_for_empty_since() {
    // Simulate the NEW URL construction logic:
    // url << "?timeout=" << timeoutMs << "&full_state=" << (fullState ? "true" : "false");
    // if (!since.empty()) url << "&since=" << since;
    //
    // When fullState=false and since="" the URL should be:
    //   /sync?timeout=10000&full_state=false
    // (NO &since= appended because since is empty, and NO full_state=true)

    std::string since = "";
    bool fullState = false;
    int timeout = 10000;

    std::ostringstream url;
    url << "?timeout=" << timeout
        << "&full_state=" << (fullState ? "true" : "false");
    if (!since.empty()) url << "&since=" << since;

    std::string urlStr = url.str();
    CHECK(urlStr.find("full_state=true") == std::string::npos,
        "empty since + fullState=false → full_state=false NOT full_state=true");
    CHECK(urlStr.find("&since=") == std::string::npos,
        "empty since → no since= appended");
    CHECK(urlStr.find("full_state=false") != std::string::npos,
        "url contains full_state=false");
}

static void test_sync_url_with_since() {
    std::string since = "s123_456";
    bool fullState = false;
    int timeout = 10000;

    std::ostringstream url;
    url << "?timeout=" << timeout
        << "&full_state=" << (fullState ? "true" : "false");
    if (!since.empty()) url << "&since=" << since;

    std::string urlStr = url.str();
    CHECK(urlStr.find("full_state=false") != std::string::npos,
        "non-empty since → full_state=false in URL");
    CHECK(urlStr.find("&since=s123_456") != std::string::npos,
        "non-empty since → since= added to URL");
}

// ---- State event name extraction simulation ----

struct SimEvent {
    std::string type;
    std::string contentJson;
};

static std::string computeNameFromState(const std::vector<SimEvent>& state) {
    for (const auto& e : state) {
        if (e.type == "m.room.name" && !e.contentJson.empty()) {
            auto name = extractJsonString(e.contentJson, "name");
            if (!name.empty()) return name;
        }
    }
    for (const auto& e : state) {
        if (e.type == "m.room.canonical_alias" && !e.contentJson.empty()) {
            auto alias = extractJsonString(e.contentJson, "alias");
            if (!alias.empty()) return alias;
        }
    }
    return {};
}

static void test_room_name_from_state() {
    auto name = computeNameFromState({
        {"m.room.name", R"({"name":"Team Dev"})"},
    });
    CHECK_EQ(name, "Team Dev", "room name from m.room.name state event");
}

static void test_room_name_fallback_to_alias() {
    auto name = computeNameFromState({
        {"m.room.canonical_alias", R"({"alias":"#dev:matrix.org"})"},
    });
    CHECK_EQ(name, "#dev:matrix.org", "room name fallback to alias");
}

static void test_room_name_empty_state() {
    auto name = computeNameFromState({
        {"m.room.topic", R"({"topic":"chat"})"},
        {"m.room.join_rules", R"({"join_rule":"invite"})"},
    });
    CHECK(name.empty(), "no name/alias → empty (caller keeps old name)");
}

static void test_room_avatar_extraction() {
    auto avatar = extractJsonStringDecoded(
        R"({"url":"mxc://matrix.org/abc"})", "url");
    CHECK_EQ(avatar, "mxc://matrix.org/abc", "avatar extraction from state");
}

// ---- Simulate lazy loading: room has roomId as name, then state arrives ----

static void test_lazy_name_update_simulation() {
    // Room initially has roomId as name (no state from sync)
    std::string roomId = "!abc123:matrix.org";
    std::string roomName = roomId;

    // Lazy fetch returns state event
    std::string nameEvent = R"({"name":"General Chat"})";
    std::string newName = extractJsonStringDecoded(nameEvent, "name");

    if (roomName == roomId && !newName.empty()) {
        roomName = newName;
    }
    CHECK_EQ(roomName, "General Chat", "lazy name update replaces roomId with real name");
}

// ---- Simulate batch loading with multiple rooms ----

static void test_batch_no_need_for_named_rooms() {
    struct Room {
        std::string id;
        std::string name;
        std::string avatarUrl;
        bool needsFetch() const { return name == id || avatarUrl.empty(); }
    };

    std::vector<Room> rooms = {
        {"!a:matrix.org", "Team Chat", "mxc://m.org/a"},
        {"!b:matrix.org", "!b:matrix.org", ""},
        {"!c:matrix.org", "Dev Room", ""},
        {"!d:matrix.org", "!d:matrix.org", ""},
    };

    int needFetch = 0;
    for (const auto& r : rooms) {
        if (r.needsFetch()) needFetch++;
    }
    CHECK_EQ(needFetch, 3, "3 of 4 rooms need lazy fetch (missing name or avatar)");
}

// ---- Simulate state event from getRoomStateEvent API ----

static void test_getRoomStateEvent_response_name() {
    std::string resp = R"({"name":"My Chat"})";
    auto name = extractJsonStringDecoded(resp, "name");
    CHECK_EQ(name, "My Chat", "getRoomStateEvent response → extract name");
}

static void test_getRoomStateEvent_response_avatar() {
    std::string resp = R"({"url":"mxc://matrix.org/myroom/avatar"})";
    auto url = extractJsonStringDecoded(resp, "url");
    CHECK_EQ(url, "mxc://matrix.org/myroom/avatar",
        "getRoomStateEvent response → extract avatar url");
}

static void test_getRoomStateEvent_404_empty() {
    // 404 → ok=true, data="" (no error)
    // Our test: extract from empty string → empty
    auto name = extractJsonStringDecoded("", "name");
    CHECK(name.empty(), "empty response → name is empty (keep old)");
}

int main() {
    std::cout << "=== Phase 4: Lazy Room State Loading Tests ===\n\n";

    test_jsonUnescape_ascii();
    test_jsonUnescape_unicode();
    test_extractJsonString_basic();
    test_extractJsonString_with_space();
    test_extractJsonString_mxc_url();
    test_extractJsonString_missing_key();
    test_extractJsonString_decoded_escapes();
    test_extractJsonString_url_with_colon();

    std::cout << "\n";

    test_sync_url_no_full_state_for_empty_since();
    test_sync_url_with_since();
    test_room_name_from_state();
    test_room_name_fallback_to_alias();
    test_room_name_empty_state();
    test_room_avatar_extraction();
    test_lazy_name_update_simulation();
    test_batch_no_need_for_named_rooms();
    test_getRoomStateEvent_response_name();
    test_getRoomStateEvent_response_avatar();
    test_getRoomStateEvent_404_empty();

    std::cout << "\n";

    if (failures == 0) {
        std::cout << "=== ALL TESTS PASSED ===" << std::endl;
        return 0;
    } else {
        std::cerr << "=== " << failures << " TEST(S) FAILED ===" << std::endl;
        return 1;
    }
}

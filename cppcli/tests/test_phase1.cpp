// tests/test_phase1.cpp — Phase 1 unit tests (no Qt, no network).
//
// Tests the parts that can be exercised without a live homeserver:
//   - progressive_native parser modules (login_flow, well_known, matrix_error, sync_models)
//   - SessionStore (in-memory SQLite)
//   - Audit classifier consistency
//
// Run:
//   cmake --build build -j4
//   ctest --test-dir build --output-on-failure

#include <cassert>
#include <iostream>
#include <string>

#include "core/session_store.hpp"

#include <progressive/login_flow.hpp>
#include <progressive/matrix_error.hpp>
#include <progressive/sync_models.hpp>
#include <progressive/well_known.hpp>
#include <progressive/json_parser.hpp>

#include "core/debug_log.hpp"

using namespace progressive::desktop;
using namespace progressive;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

static void test_well_known() {
    std::cout << "--- test_well_known ---\n";
    auto r = progressive::parseServerDiscovery(
        R"({"m.homeserver":{"base_url":"https://matrix.example.org"}})");
    CHECK(r.isValid, "well-known with homeserver → valid");
    CHECK(r.homeserverBaseUrl == "https://matrix.example.org", "extracts base_url");

    auto r2 = progressive::parseServerDiscovery("{}");
    CHECK(!r2.isValid, "empty well-known → invalid");

    auto formatted = progressive::formatServerUrl("matrix.org");
    CHECK(formatted == "https://matrix.org", "formatServerUrl adds https://");

    auto formatted2 = progressive::formatServerUrl("https://matrix.org/");
    CHECK(formatted2 == "https://matrix.org", "formatServerUrl strips trailing slash");
}

static void test_login_flow_parse() {
    std::cout << "--- test_login_flow_parse ---\n";
    auto r = progressive::parseLoginFlows(
        R"({"flows":[{"type":"m.login.password"},{"type":"m.login.sso","identity_providers":[{"id":"google","name":"Google"}]}]})");
    CHECK(r.flows.size() >= 2, "parses 2 flows");
}

static void test_matrix_error_parse() {
    std::cout << "--- test_matrix_error_parse ---\n";
    auto e = progressive::parseMatrixErrorJson(
        R"({"errcode":"M_FORBIDDEN","error":"You are not allowed to do that"})");
    CHECK(e.valid, "parsed error is valid");
    CHECK(e.code == "M_FORBIDDEN", "extracts errcode");
    CHECK(e.message == "You are not allowed to do that", "extracts error message");
}

static void test_sync_parse_minimal() {
    std::cout << "--- test_sync_parse_minimal ---\n";
    // Empty sync response — just nextBatch
    auto r = progressive::parseSyncResponse(R"({"next_batch":"s1_123","rooms":{}})");
    CHECK(r.nextBatch == "s1_123", "extracts next_batch");
    CHECK(r.rooms.join.empty(), "no joined rooms");

    // Sync with one joined room and one timeline event
    auto r2 = progressive::parseSyncResponse(
        R"({"next_batch":"s1_124","rooms":{"join":{"!room:server":{"timeline":{"events":[{"type":"m.room.message","content":{"body":"hi","msgtype":"m.text"}}],"limited":false}}}}})");
    CHECK(r2.nextBatch == "s1_124", "extracts next_batch 2");
    CHECK(r2.rooms.join.size() == 1, "one joined room");
}

static void test_session_store_memory() {
    std::cout << "--- test_session_store_memory ---\n";
    SessionStore s;
    bool ok = s.open(":memory:");
    CHECK(ok, "open :memory:");

    AccountInfo a{"@user:server", "DEV1", "https://matrix.org", "tok_abc", "refresh_xyz"};
    CHECK(s.saveAccount(a), "save account");

    auto loaded = s.loadAccount();
    CHECK(loaded.has_value(), "load account returns value");
    CHECK(loaded->userId == "@user:server", "userId persisted");
    CHECK(loaded->deviceId == "DEV1", "deviceId persisted");
    CHECK(loaded->homeserverUrl == "https://matrix.org", "homeserverUrl persisted");
    CHECK(loaded->accessToken == "tok_abc", "accessToken persisted");
    CHECK(loaded->refreshToken == "refresh_xyz", "refreshToken persisted");

    CHECK(s.saveSyncToken("@user:server", "since_123"), "save sync token");
    auto tok = s.loadSyncToken("@user:server");
    CHECK(tok.has_value(), "load sync token returns value");
    CHECK(*tok == "since_123", "sync token persisted");
    CHECK(s.saveSyncToken("@other:server", "since_999"), "per-user: second account saved");
    CHECK(s.loadSyncToken("@user:server") && *s.loadSyncToken("@user:server") == "since_123",
          "per-user: accounts keep their own sync positions");

    CHECK(s.clearSyncToken("@user:server"), "clear sync token");
    CHECK(!s.loadSyncToken("@user:server").has_value(), "sync token cleared");
    CHECK(s.loadSyncToken("@other:server") && *s.loadSyncToken("@other:server") == "since_999",
          "per-user: other account's token survives");

    CHECK(s.clearAccount("@user:server"), "clear account");
    CHECK(!s.loadAccount().has_value(), "account cleared");
}

// The in-app log ring must round-trip: LOG writes must appear in the
// snapshot the log viewer reads (regression: two separate static rings
// made the viewer permanently empty).
static void test_log_ring() {
    const size_t before = snapshotLogRing().size();
    LOG(LogChannel::E2EE, "ring-test-%d", 42);
    LOG(LogChannel::DBG, "ring-test-two");
    auto lines = snapshotLogRing();
    CHECK(lines.size() == before + 2, "log ring round-trips writes");
    bool found = false;
    for (const auto& l : lines) {
        if (l.channel == LogChannel::E2EE && l.text.find("ring-test-42") != std::string::npos)
            found = true;
    }
    CHECK(found, "log ring preserves channel + text");
}

int main() {
    std::cout << "=== Phase 1 unit tests ===\n";
    test_well_known();
    test_login_flow_parse();
    test_matrix_error_parse();
    test_sync_parse_minimal();
    test_session_store_memory();
    test_log_ring();

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "ALL PASSED\n";
        return 0;
    }
    std::cerr << failures << " FAILURE(S)\n";
    return 1;
}

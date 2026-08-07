// src/core/matrix_client.cpp — implementation.

#include "matrix_client.hpp"
#include "http_client.hpp"
#include "core/debug_log.hpp"
#include "core/crypto/media_crypto.hpp"

#include <progressive/login_flow.hpp>
#include <progressive/matrix_error.hpp>
#include <progressive/sync_models.hpp>
#include <progressive/well_known.hpp>
#include <progressive/json_parser.hpp>
#include <progressive/content_utils.hpp>

#include <simdjson.h>
#include <sstream>
#include <chrono>
#include <atomic>
#include <ctime>

namespace progressive::desktop {

namespace {

// ---- Helper: URL-encode a room ID or alias for use in paths.
std::string urlEncodePath(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '#') out += "%23";
        else if (c == ' ') out += "%20";
        else if (c == '/') out += "%2F";  // SSSS key ids (base64) can contain '/'
        else out += c;
    }
    return out;
}

// ---- Helper: generate unique txn ID ----
// Percent-encode a query value (unreserved chars only) — upload filenames
// with spaces/Cyrillic/& must not break the URL.
static std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 15];
        }
    }
    return out;
}



// ---- Helper: JSON escape ----
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// Build a JSON body for m.login.password login.
std::string buildLoginBody(const std::string& username,
                           const std::string& password,
                           const std::string& deviceId) {
    std::ostringstream o;
    o << R"({"type":"m.login.password","identifier":{"type":"m.id.user","user":")"
      << username << R"("},"password":")" << password << R"(")";
    if (!deviceId.empty()) {
        o << R"(,"device_id":")" << deviceId << R"(")";
    }
    // Request a refresh token (MSC2918) — server may ignore.
    o << R"(,"refresh_token":true})";
    return o.str();
}

// Parse POST /login response → AccountInfo.
// Uses progressive::parseJsonStringValue for the top-level fields.
// (parseCredentials isn't a single public function in progressive_native;
//  the JSON is simple enough to read directly.)
AccountInfo parseLoginResponse(const std::string& json,
                                const std::string& homeserverUrl) {
    AccountInfo a;
    a.userId        = progressive::parseJsonStringValue(json, "user_id");
    a.accessToken   = progressive::parseJsonStringValue(json, "access_token");
    a.refreshToken  = progressive::parseJsonStringValue(json, "refresh_token");
    a.deviceId      = progressive::parseJsonStringValue(json, "device_id");
    a.homeserverUrl = homeserverUrl;
    return a;
}

} // namespace

MatrixClient::MatrixClient() {
    httpInit();
}

MatrixClient::~MatrixClient() {
    // httpCleanup is global — defer to application shutdown, not per-client.
}

std::unordered_map<std::string, std::string> MatrixClient::authHeaders() const {
    std::unordered_map<std::string, std::string> h;
    auto acct = account();
    if (!acct.accessToken.empty()) {
        h["Authorization"] = "Bearer " + acct.accessToken;
    }
    h["Content-Type"] = "application/json";
    h["Accept"] = "application/json";
    return h;
}


// Unique transaction-id for /send calls (time + counter — never collides
// within a second, unlike a bare timestamp).
std::string genTxnId(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    uint64_t t = static_cast<uint64_t>(std::time(nullptr)) * 1000 + (counter.fetch_add(1) % 1000);
    return prefix + std::to_string(t);
}

ApiResult<std::string> MatrixClient::discoverHomeserver(const std::string& userInput) {
    ApiResult<std::string> r;
    // Step 1: format the user input as a URL.
    std::string url = progressive::formatServerUrl(userInput);

    // Step 2: try .well-known
    auto resp = httpGet(url + "/.well-known/matrix/client",
                        {{"Accept", "application/json"}}, 10000);
    if (resp.success) {
        auto d = progressive::parseServerDiscovery(resp.body);
        if (d.isValid && !d.homeserverBaseUrl.empty()) {
            r.ok = true;
            r.data = d.homeserverBaseUrl;
            r.httpStatus = resp.statusCode;
            return r;
        }
    }
    // Step 3: fall back to the formatted URL (no well-known).
    // Validate it later via getVersions().
    r.ok = true;
    r.data = url;
    r.httpStatus = resp.statusCode;
    return r;
}

ApiResult<std::string> MatrixClient::getVersions() {
    ApiResult<std::string> r;
    if (account().homeserverUrl.empty()) {
        r.error.message = "no homeserver URL set";
        return r;
    }
    auto resp = httpGet(account().homeserverUrl + "/_matrix/client/versions",
                        authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.ok = true;
        r.data = resp.body;
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<progressive::LoginAuthFlowsResult> MatrixClient::getLoginFlows() {
    ApiResult<progressive::LoginAuthFlowsResult> r;
    if (account().homeserverUrl.empty()) {
        r.error.message = "no homeserver URL set";
        return r;
    }
    auto resp = httpGet(account().homeserverUrl + "/_matrix/client/v3/login",
                        authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseLoginFlows(resp.body);
        r.ok = true;
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<AccountInfo> MatrixClient::loginWithPassword(const std::string& username,
                                                        const std::string& password,
                                                        const std::string& deviceId) {
    ApiResult<AccountInfo> r;
    if (account().homeserverUrl.empty()) {
        r.error.message = "no homeserver URL set (call discoverHomeserver first)";
        return r;
    }
    std::string body = buildLoginBody(username, password, deviceId);
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/login",
                         body, authHeaders(), 30000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = parseLoginResponse(resp.body, account().homeserverUrl);
        if (r.data.isValid() ||
            (!r.data.userId.empty() && !r.data.accessToken.empty())) {
            setAccount(r.data);
            r.ok = true;
        } else {
            r.error.code = progressive::ErrorCode::M_UNKNOWN;
            r.error.message = "login response missing user_id or access_token";
        }
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<bool> MatrixClient::logout() {
    ApiResult<bool> r;
    if (!isLoggedIn()) {
        r.ok = true;
        r.data = true;
        return r;
    }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/logout",
                         "{}", authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    r.ok = resp.success;
    r.data = resp.success;
    if (!resp.success && !resp.body.empty()) {
        r.error = progressive::parseMatrixErrorJson(resp.body);
    }
    // Clear local state regardless of server response.
    setAccount(AccountInfo{});
    return r;
}

ApiResult<AccountInfo> MatrixClient::refreshAccessToken(const std::string& refreshToken) {
    ApiResult<AccountInfo> r;
    if (account().homeserverUrl.empty() || refreshToken.empty()) {
        r.error.message = "no homeserver URL or refresh token";
        return r;
    }
    std::string body = R"({"refresh_token":")" + refreshToken + R"("})";
    // Refresh endpoint: authenticate with the refresh token, not the
    // (possibly expired) access token. Use minimal headers.
    std::unordered_map<std::string, std::string> hdrs;
    hdrs["Content-Type"] = "application/json";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/refresh",
                         body, hdrs, 15000);
    LOG(LogChannel::NET, "/refresh http=%d body=[%.200s]",
        resp.statusCode,
        resp.body.data() ? resp.body.c_str() : "(null)");
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = parseLoginResponse(resp.body, account().homeserverUrl);
        r.ok = r.data.isValid();
        if (!r.ok) r.error.message = "refresh response missing access_token";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::sendMessage(const std::string& roomId,
                                                  const std::string& body,
                                                  const std::string& msgtype) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }
    // Build a unique transaction ID: timestamp + counter.
    static std::atomic<uint64_t> txnCounter{0};
    uint64_t txn = static_cast<uint64_t>(std::time(nullptr)) * 1000 +
                    (txnCounter.fetch_add(1) % 1000);
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/"
        << urlEncodePath(roomId) << "/send/m.room.message/" << "pd" << txn;

    // Escape body for JSON
    std::string escaped;
    escaped.reserve(body.size() + 8);
    for (char c : body) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    escaped += buf;
                } else {
                    escaped += c;
                }
        }
    }

    std::ostringstream jsonBody;
    jsonBody << R"({"msgtype":")" << msgtype << R"(","body":")" << escaped << R"("})";

    auto resp = httpPut(url.str(), jsonBody.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "event_id");
        r.ok = !r.data.empty();
        if (!r.ok) r.error.message = "send: no event_id in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::getMessages(const std::string& roomId,
                                                    const std::string& from,
                                                    int limit) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/"
        << roomId << "/messages?dir=b&limit=" << limit;
    if (!from.empty()) url << "&from=" << from;

    auto resp = httpGet(url.str(), authHeaders(), 30000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.ok = true;
        r.data = resp.body;
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<bool> MatrixClient::setReadMarker(const std::string& roomId,
                                              const std::string& eventId) {
    ApiResult<bool> r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }
    if (invisibleMode_) {
        r.ok = true; r.data = true;
        return r;
    }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/" << roomId << "/read_markers";
    std::string body = R"({"m.read":")" + eventId + R"("})";
    auto resp = httpPost(url.str(), body, authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    r.ok = resp.success;
    r.data = resp.success;
    if (!resp.success && !resp.body.empty()) {
        r.error = progressive::parseMatrixErrorJson(resp.body);
    }
    return r;
}

ApiResult<std::string> MatrixClient::createRoom(const std::string& name,
                                                  const std::string& topic,
                                                  bool isDirect,
                                                  const std::vector<std::string>& inviteUserIds,
                                                  bool encrypt) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }

    // Build JSON body
    std::ostringstream o;
    o << R"({"name":")" << name << R"(",)";
    if (!topic.empty()) {
        o << R"("topic":")" << topic << R"(",)";
    }
    o << R"("is_direct":)" << (isDirect ? "true" : "false");
    if (!inviteUserIds.empty()) {
        o << R"(,"invite":[)";
        for (size_t i = 0; i < inviteUserIds.size(); ++i) {
            if (i > 0) o << ",";
            o << R"(")" << inviteUserIds[i] << R"(")";
        }
        o << "]";
    }
    if (encrypt) {
        o << R"(,"initial_state":[{"type":"m.room.encryption",)";
        o << R"("state_key":"",)";
        o << R"("content":{"algorithm":"m.megolm.v1.aes-sha2"}}])";
    }
    o << R"(,"visibility":"private"})";

    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/createRoom",
                         o.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "room_id");
        r.ok = !r.data.empty();
        if (!r.ok) r.error.message = "createRoom: no room_id in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::startDirectMessage(const std::string& userId, bool encrypt) {
    // For now: create a new direct room with this user.
    // Room name: the other user's displayname or ID (simplified to their localpart)
    std::string otherName = userId;
    if (otherName[0] == '@') {
        auto colon = otherName.find(':');
        if (colon != std::string::npos) otherName = otherName.substr(1, colon - 1);
        else otherName = otherName.substr(1);
    }
    return createRoom(otherName, "", true, {userId}, encrypt);
}

ApiResult<std::string> MatrixClient::searchUsers(const std::string& query, int limit) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }
    std::ostringstream o;
    o << R"({"search_term":")" << query << R"(","limit":)" << limit << "}";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/user_directory/search",
                         o.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.ok = true;
        r.data = resp.body;
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::getUserProfile(const std::string& userId) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/profile/" << userId;
    auto resp = httpGet(url.str(), authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.ok = true;
        r.data = resp.body;
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::joinRoom(const std::string& roomIdOrAlias,
                                                 const std::vector<std::string>& viaServers) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::fprintf(stderr, "[join] joinRoom called: %s\n", roomIdOrAlias.c_str());
    std::ostringstream body;
    if (!viaServers.empty()) {
        // Matrix spec: body is {"via": ["server1", "server2"]}
        body << "{\"via\":[";
        for (size_t i = 0; i < viaServers.size(); ++i) {
            if (i > 0) body << ",";
            body << "\"" << jsonEscape(viaServers[i]) << "\"";
        }
        body << "]}";
    } else body << "{}";
    // URL-encode the room ID/alias. Aliases start with '#' which MUST be
    // %23 in URLs. Room IDs start with '!' which is usually fine but
    // urlEncodePath handles spaces too.
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/join/" + urlEncodePath(roomIdOrAlias),
                         body.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "room_id");
        r.ok = !r.data.empty();
        if (!r.ok) r.error.message = "join: no room_id in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<bool> MatrixClient::inviteUser(const std::string& roomId,
                                              const std::string& userId) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string body = "{\"user_id\":\"" + jsonEscape(userId) + "\"}";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/rooms/"
                         + urlEncodePath(roomId) + "/invite", body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    LOG(LogChannel::NET, "inviteUser: room=%s user=%s ok=%d http=%d",
        roomId.c_str(), userId.c_str(), r.ok ? 1 : 0, r.httpStatus);
    return r;
}

ApiResult<bool> MatrixClient::leaveRoom(const std::string& roomId) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) + "/leave",
                         "{}", authHeaders(), 15000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    LOG(LogChannel::NET, "leaveRoom: room=%s ok=%d http=%d body=%.200s",
        roomId.c_str(), r.ok ? 1 : 0, r.httpStatus, resp.body.c_str());
    return r;
}

ApiResult<bool> MatrixClient::forgetRoom(const std::string& roomId) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/rooms/"
                         + urlEncodePath(roomId) + "/forget",
                         "{}", authHeaders(), 15000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<std::string> MatrixClient::deleteDevice(const std::string& deviceId,
                                                    const std::string& password) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::ostringstream body;
    body << "{\"auth\":{\"type\":\"m.login.password\",\"identifier\":"
          "{\"type\":\"m.id.user\",\"user\":\""
         << account().userId << "\"},\"password\":\""
         << jsonEscape(password) << "\"},\"devices\":[\"" << deviceId << "\"]}";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/delete_devices",
                         body.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.ok = true;
        r.data = resp.body;
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        if (r.error.message.empty()) r.error.message = resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::sendReaction(const std::string& roomId,
                                                    const std::string& eventId,
                                                    const std::string& emoji) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string txn = genTxnId("react");
    std::ostringstream body;
    body << "{\"m.relates_to\":{\"rel_type\":\"m.annotation\",\"event_id\":\""
         << jsonEscape(eventId) << "\",\"key\":\"" << jsonEscape(emoji) << "\"}}";
    auto resp = httpPut(account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) +
                         "/send/m.reaction/" + txn, body.str(), authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "event_id");
        r.ok = !r.data.empty();
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    }
    return r;
}

ApiResult<bool> MatrixClient::redactEvent(const std::string& roomId,
                                            const std::string& eventId,
                                            const std::string& reason) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string txn = genTxnId("redact");
    std::string body = reason.empty() ? "{}"
        : "{\"reason\":\"" + jsonEscape(reason) + "\"}";
    // PUT /_matrix/client/v3/rooms/{roomId}/redact/{eventId}/{txnId}
    auto resp = httpPut(account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) +
                        "/redact/" + eventId + "/" + txn, body, authHeaders(), 10000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<bool> MatrixClient::pinMessage(const std::string& roomId, const std::string& eventId) {
    // Fetch current pinned events, append the new one, PUT back.
    auto state = getRoomState(roomId);
    if (!state.ok) { ApiResult<bool> r; r.error = state.error; return r; }
    // Find existing pinned list
    std::string pinnedJson;
    auto pos = state.data.find("\"pinned\"");
    if (pos != std::string::npos) {
        auto arrStart = state.data.find('[', pos);
        auto arrEnd = state.data.find(']', arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            pinnedJson = state.data.substr(arrStart, arrEnd - arrStart + 1);
        }
    }
    // Append eventId
    std::string newPinned;
    if (pinnedJson.empty() || pinnedJson == "[]") {
        newPinned = "[\"" + jsonEscape(eventId) + "\"]";
    } else {
        // Insert before closing ]
        newPinned = pinnedJson.substr(0, pinnedJson.size() - 1) + ",\"" + jsonEscape(eventId) + "\"]";
    }
    std::string body = "{\"pinned\":" + newPinned + "}";
    return sendStateEvent(roomId, "m.room.pinned_events", "", body);
}

ApiResult<bool> MatrixClient::unpinMessage(const std::string& roomId, const std::string& eventId) {
    auto state = getRoomState(roomId);
    if (!state.ok) { ApiResult<bool> r; r.error = state.error; return r; }
    // Find pinned list and remove eventId
    std::string body = "{\"pinned\":[]}";
    auto pos = state.data.find("\"pinned\"");
    if (pos != std::string::npos) {
        auto arrStart = state.data.find('[', pos);
        auto arrEnd = state.data.find(']', arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string arrStr = state.data.substr(arrStart + 1, arrEnd - arrStart - 1);
            // Split by comma, filter out eventId
            std::vector<std::string> ids;
            size_t start = 0;
            while (start < arrStr.size()) {
                auto q1 = arrStr.find('"', start);
                if (q1 == std::string::npos) break;
                auto q2 = arrStr.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                auto id = arrStr.substr(q1 + 1, q2 - q1 - 1);
                if (id != eventId) ids.push_back(id);
                start = q2 + 1;
            }
            std::string newList = "[";
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i > 0) newList += ",";
                newList += "\"" + jsonEscape(ids[i]) + "\"";
            }
            newList += "]";
            body = "{\"pinned\":" + newList + "}";
        }
    }
    return sendStateEvent(roomId, "m.room.pinned_events", "", body);
}

ApiResult<std::string> MatrixClient::getRoomState(const std::string& roomId) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpGet(account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) + "/state",
                        authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
            r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage; }
    return r;
}

ApiResult<std::string> MatrixClient::getRoomStateEvent(const std::string& roomId,
                                                          const std::string& eventType,
                                                          const std::string& stateKey) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string url = account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId)
                      + "/state/" + eventType;
    if (!stateKey.empty()) url += "/" + stateKey;
    auto resp = httpGet(url, authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else {
        // 404 is not an error for state events — means the state isn't set
        if (resp.statusCode == 404) { r.ok = true; r.data = ""; }
        else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
               r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage; }
    }
    return r;
}

ApiResult<bool> MatrixClient::sendStateEvent(const std::string& roomId,
                                              const std::string& eventType,
                                              const std::string& stateKey,
                                              const std::string& bodyJson) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string url = account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) +
                      "/state/" + eventType;
    if (!stateKey.empty()) url += "/" + stateKey;
    auto resp = httpPut(url, bodyJson, authHeaders(), 10000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<bool> MatrixClient::setRoomTopic(const std::string& roomId, const std::string& topic) {
    std::string body = "{\"topic\":\"" + jsonEscape(topic) + "\"}";
    return sendStateEvent(roomId, "m.room.topic", "", body);
}

ApiResult<bool> MatrixClient::setRoomName(const std::string& roomId, const std::string& name) {
    std::string body = "{\"name\":\"" + jsonEscape(name) + "\"}";
    return sendStateEvent(roomId, "m.room.name", "", body);
}

ApiResult<std::string> MatrixClient::getRoomMembers(const std::string& roomId) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string url = account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) + "/members";
    LOG(LogChannel::NET, "getRoomMembers: room=%.60s", roomId.c_str());
    auto resp = httpGet(url, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body); }
    LOG(LogChannel::NET, "getRoomMembers: room=%.60s http=%d ok=%d err=%.200s",
        roomId.c_str(), r.httpStatus, r.ok ? 1 : 0, r.error.message.c_str());
    return r;
}

ApiResult<bool> MatrixClient::kickUser(const std::string& roomId, const std::string& userId, const std::string& reason) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string body = "{\"user_id\":\"" + jsonEscape(userId) + "\"";
    if (!reason.empty()) body += ",\"reason\":\"" + jsonEscape(reason) + "\"";
    body += "}";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) + "/kick",
                         body, authHeaders(), 10000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<bool> MatrixClient::banUser(const std::string& roomId, const std::string& userId, const std::string& reason) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string body = "{\"user_id\":\"" + jsonEscape(userId) + "\"";
    if (!reason.empty()) body += ",\"reason\":\"" + jsonEscape(reason) + "\"";
    body += "}";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) + "/ban",
                         body, authHeaders(), 10000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<bool> MatrixClient::unbanUser(const std::string& roomId, const std::string& userId) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string body = "{\"user_id\":\"" + jsonEscape(userId) + "\"}";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/rooms/" + urlEncodePath(roomId) + "/unban",
                         body, authHeaders(), 10000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<bool> MatrixClient::setUserPowerLevel(const std::string& roomId, const std::string& userId, int level) {
    // GET current m.room.power_levels, modify the user's level, PUT back.
    // Uses simdjson to parse safely — preserves all other power levels.
    auto state = getRoomState(roomId);
    ApiResult<bool> r;
    if (!state.ok) { r.error = state.error; return r; }

    // Parse state events array, find m.room.power_levels
    simdjson::dom::parser parser;
    auto rootResult = parser.parse(state.data);
    if (rootResult.error() != simdjson::SUCCESS) {
        r.error.message = "failed to parse state response";
        return r;
    }
    auto arrResult = rootResult.value().get_array();
    if (arrResult.error() != simdjson::SUCCESS) {
        r.error.message = "state response is not an array";
        return r;
    }
    std::string plContent;
    bool found = false;
    for (auto evt : arrResult.value()) {
        auto t = evt["type"].get_string();
        if (t.error() == simdjson::SUCCESS && t.value() == "m.room.power_levels") {
            auto contentRes = evt["content"];
            if (contentRes.error() == simdjson::SUCCESS) {
                plContent = simdjson::to_string(contentRes.value());
                found = true;
            }
            break;
        }
    }
    if (!found) {
        r.error.message = "no m.room.power_levels in state";
        return r;
    }

    // Parse the power_levels content, modify the user's level, re-serialize.
    // We need a second parse to navigate into "users" object.
    simdjson::dom::parser plParser;
    auto plRoot = plParser.parse(plContent);
    if (plRoot.error() != simdjson::SUCCESS) {
        r.error.message = "failed to parse power_levels content";
        return r;
    }

    // Build new JSON by string manipulation since simdjson DOM is read-only.
    // Find the "users" object, modify or insert the user's level.
    auto usersPos = plContent.find("\"users\"");
    if (usersPos == std::string::npos) {
        // No users map — add one
        if (plContent.back() == '}') {
            std::string ins = ",\"users\":{\"" + userId + "\":" + std::to_string(level) + "}";
            plContent.insert(plContent.size() - 1, ins);
        }
    } else {
        // Find the user's existing entry inside the users object
        auto usersObjStart = plContent.find('{', usersPos);
        if (usersObjStart == std::string::npos) {
            r.error.message = "malformed users object in power_levels";
            return r;
        }
        // Find end of users object by depth counting
        int depth = 0;
        size_t usersObjEnd = usersObjStart;
        for (size_t i = usersObjStart; i < plContent.size(); ++i) {
            if (plContent[i] == '{') depth++;
            else if (plContent[i] == '}') { depth--; if (depth == 0) { usersObjEnd = i; break; } }
        }
        std::string userNeedle = "\"" + userId + "\"";
        auto userPos = plContent.find(userNeedle, usersObjStart);
        if (userPos != std::string::npos && userPos < usersObjEnd) {
            // Replace existing level — find the colon and number after it
            auto colonPos = plContent.find(':', userPos + userNeedle.size());
            if (colonPos == std::string::npos || colonPos > usersObjEnd) {
                r.error.message = "malformed user entry in power_levels";
                return r;
            }
            size_t valueEnd = colonPos + 1;
            while (valueEnd < plContent.size() &&
                   (std::isdigit(static_cast<unsigned char>(plContent[valueEnd])) ||
                    plContent[valueEnd] == '-')) valueEnd++;
            plContent.replace(colonPos + 1, valueEnd - colonPos - 1, std::to_string(level));
        } else {
            // Insert new user entry before closing } of users object
            std::string ins = (usersObjEnd - usersObjStart > 1) ? "," : "";
            ins += userNeedle + ":" + std::to_string(level);
            plContent.insert(usersObjEnd, ins);
        }
    }

    return sendStateEvent(roomId, "m.room.power_levels", "", plContent);
}

ApiResult<std::string> MatrixClient::getThreads(const std::string& roomId, const std::string& from, int limit) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::ostringstream url;
    // /threads endpoint is v1 per Matrix spec
    url << account().homeserverUrl << "/_matrix/client/v1/rooms/" << urlEncodePath(roomId)
        << "/threads?limit=" << limit;
    if (!from.empty()) url << "&from=" << from;
    auto resp = httpGet(url.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body); }
    return r;
}

ApiResult<std::string> MatrixClient::getThreadReplies(const std::string& roomId, const std::string& rootEventId) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    // /relations endpoint is v1 per Matrix spec.
    // Returns {original_event: ..., chunk: [...replies...]}
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v1/rooms/" << urlEncodePath(roomId)
        << "/relations/" << urlEncodePath(rootEventId) << "/m.thread?limit=50&dir=f";
    auto resp = httpGet(url.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body); }
    return r;
}

ApiResult<std::string> MatrixClient::getEvent(const std::string& roomId, const std::string& eventId) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/" << urlEncodePath(roomId)
        << "/event/" << urlEncodePath(eventId);
    auto resp = httpGet(url.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body); }
    return r;
}

ApiResult<std::string> MatrixClient::searchPublicRooms(const std::string& server, const std::string& query, int limit, const std::string& from) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::ostringstream body;
    body << "{";
    if (!server.empty()) body << "\"server\":\"" << jsonEscape(server) << "\",";
    body << "\"limit\":" << limit;
    if (!from.empty()) body << ",\"from\":\"" << jsonEscape(from) << "\"";
    if (!query.empty()) body << ",\"filter\":{\"generic_search_term\":\"" << jsonEscape(query) << "\"}";
    body << "}";
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/publicRooms",
                         body.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body); }
    return r;
}

ApiResult<std::string> MatrixClient::getSpaceHierarchy(const std::string& spaceId, int maxDepth) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v1/rooms/" << spaceId
        << "/hierarchy?max_depth=" << maxDepth;
    auto resp = httpGet(url.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body); }
    return r;
}

ApiResult<std::vector<uint8_t>> MatrixClient::downloadMedia(const std::string& mxcUrl, int width, int height) {
    ApiResult<std::vector<uint8_t>> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    // Resolve mxc:// to HTTP URL using progressive::resolveMxcDownloadUrl / resolveMxcThumbnailUrl
    std::string httpUrl;
    if (width > 0 && height > 0) {
        httpUrl = progressive::resolveMxcThumbnailUrl(mxcUrl, account().homeserverUrl, width, height, "scale");
    } else {
        httpUrl = progressive::resolveMxcDownloadUrl(mxcUrl, account().homeserverUrl);
    }
    if (httpUrl.empty() || httpUrl == mxcUrl) {
        r.error.message = "invalid mxc URL";
        return r;
    }
    LOG(LogChannel::NET, "downloadMedia: mxc=%.120s -> http=%d ok=%d size=%zu err=%.120s",
        mxcUrl.c_str(), 0, 0, (size_t)0, httpUrl.c_str());
    auto resp = httpGet(httpUrl, authHeaders(), 30000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.ok = true;
        r.data.assign(resp.body.begin(), resp.body.end());
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    LOG(LogChannel::NET, "downloadMedia: DONE mxc=%.120s status=%d ok=%d size=%zu err=%.120s",
        mxcUrl.c_str(), r.httpStatus, r.ok ? 1 : 0, r.data.size(), r.error.message.c_str());
    return r;
}

ApiResult<std::vector<uint8_t>> MatrixClient::downloadMediaEncrypted(
    const std::string& mxcUrl, const std::string& keyB64,
    const std::string& ivB64, const std::string& shaB64) {
    ApiResult<std::vector<uint8_t>> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto fetched = downloadMedia(mxcUrl, 0, 0);
    if (!fetched.ok) {
        r.httpStatus = fetched.httpStatus;
        r.error = fetched.error;
        return r;
    }
    auto plain = progressive::desktop::decryptMedia(fetched.data, keyB64, ivB64, shaB64);
    if (plain.empty()) {
        r.error.message = "media decryption failed (bad key or sha256 mismatch)";
        LOG(LogChannel::NET, "downloadMediaEncrypted: DECRYPT FAILED mxc=%.120s", mxcUrl.c_str());
        return r;
    }
    r.ok = true;
    r.data = std::move(plain);
    LOG(LogChannel::NET, "downloadMediaEncrypted: ok mxc=%.120s size=%zu", mxcUrl.c_str(), r.data.size());
    return r;
}

ApiResult<progressive::SyncResponse> MatrixClient::sync(const std::string& since,
                                                        int timeoutMs) {
    ApiResult<progressive::SyncResponse> r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/sync"
        << "?timeout=" << timeoutMs
        << "&full_state=false";
    if (!since.empty()) url << "&since=" << since;

    // The long-poll timeout in the URL is server-side; the HTTP timeout
    // must be slightly longer so we get the response.
    auto resp = httpGet(url.str(), authHeaders(), timeoutMs + 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseSyncResponse(resp.body);
        r.ok = true;
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

MatrixClient::FastSyncResult MatrixClient::syncFast(const std::string& since,
                                                     int timeoutMs,
                                                     bool fullState) {
    FastSyncResult r;
    if (!isLoggedIn()) {
        r.error.message = "not logged in";
        return r;
    }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/sync"
        << "?timeout=" << timeoutMs
        << "&full_state=" << (fullState ? "true" : "false");
    if (!since.empty()) url << "&since=" << since;

    auto resp = httpGet(url.str(), authHeaders(), timeoutMs + 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        std::string err;
        r.data = parseSyncResponseFast(std::move(resp.body), err, account().deviceId);
        r.ok = err.empty();
        if (!r.ok) r.error.message = std::move(err);
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

void MatrixClient::setAccount(const AccountInfo& acct) {
    accountPtr_.store(std::make_shared<AccountInfo>(acct));
}

bool MatrixClient::persistSession() {
    LOG(LogChannel::E2EE, "persistSession: ENTER store=%p isLoggedIn=%d",
        (void*)sessionStore_, isLoggedIn() ? 1 : 0);
    if (!sessionStore_) {
        LOG(LogChannel::E2EE, "persistSession: FAIL — sessionStore_ is NULL");
        return false;
    }
    return sessionStore_->saveAccount(account());
}

bool MatrixClient::loadSavedSession() {
    if (!sessionStore_) return false;
    auto acct = sessionStore_->loadAccount();
    if (acct && !acct->userId.empty() && !acct->accessToken.empty()) {
        setAccount(*acct);
        return true;
    }
    return false;
}

// ---- E2EE endpoints ----

ApiResult<std::string> MatrixClient::uploadKeys(const std::string& body) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/keys/upload",
                         body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
           r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage; }
    return r;
}

ApiResult<bool> MatrixClient::setAccountData(const std::string& type,
                                             const std::string& contentJson) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    // The type is URL-encoded: SSSS key ids are base64 and can contain '/'
    // (a literal '/' would break the path — the live SSSS test caught this).
    auto resp = httpPut(account().homeserverUrl + "/_matrix/client/v3/user/"
                        + account().userId + "/account_data/" + urlEncodePath(type),
                        contentJson, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; }
    else { r.error.message = resp.errorMessage.empty() ? "account_data failed" : resp.errorMessage; }
    return r;
}

ApiResult<std::string> MatrixClient::getAccountData(const std::string& type) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpGet(account().homeserverUrl + "/_matrix/client/v3/user/"
                        + account().userId + "/account_data/" + urlEncodePath(type),
                        authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.error.message = resp.errorMessage.empty() ? "account_data get failed" : resp.errorMessage; }
    return r;
}

ApiResult<std::string> MatrixClient::uploadDeviceSigningKeys(const std::string& body) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/keys/device_signing/upload",
                         body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.data = resp.body;  // keep the body (UIA challenge) for the caller
           r.error.message = resp.errorMessage.empty() ? "device_signing/upload failed" : resp.errorMessage; }
    return r;
}

ApiResult<std::string> MatrixClient::uploadSignatures(const std::string& body) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/keys/signatures/upload",
                         body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.error = progressive::parseMatrixErrorJson(resp.body);
           r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage; }
    LOG(LogChannel::NET, "uploadSignatures: ok=%d http=%d failures=%.300s",
        r.ok ? 1 : 0, r.httpStatus, resp.body.c_str());
    return r;
}

ApiResult<std::string> MatrixClient::createRoomKeysVersion(const std::string& body) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/room_keys/version",
                         body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.error = progressive::parseMatrixErrorJson(resp.body); }
    LOG(LogChannel::NET, "createRoomKeysVersion: ok=%d http=%d body=%.200s",
        r.ok ? 1 : 0, r.httpStatus, resp.body.c_str());
    return r;
}

ApiResult<std::string> MatrixClient::uploadRoomKeys(const std::string& body,
                                                        const std::string& version) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    // The spec: the version is a QUERY PARAMETER (the server stores into it).
    auto resp = httpPut(account().homeserverUrl + "/_matrix/client/v3/room_keys/keys?version="
                        + urlEncodePath(version), body, authHeaders(), 30000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.error = progressive::parseMatrixErrorJson(resp.body); }
    LOG(LogChannel::NET, "uploadRoomKeys: ok=%d http=%d body=%.200s",
        r.ok ? 1 : 0, r.httpStatus, resp.body.c_str());
    return r;
}

ApiResult<std::string> MatrixClient::getRoomKeysVersion(const std::string& version) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpGet(account().homeserverUrl + "/_matrix/client/v3/room_keys/version/"
                        + urlEncodePath(version), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.error = progressive::parseMatrixErrorJson(resp.body); }
    LOG(LogChannel::NET, "getRoomKeysVersion: ok=%d http=%d body=%.200s",
        r.ok ? 1 : 0, r.httpStatus, resp.body.c_str());
    return r;
}

ApiResult<std::string> MatrixClient::getRoomKeysVersions() {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpGet(account().homeserverUrl + "/_matrix/client/v3/room_keys/version",
                        authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.error = progressive::parseMatrixErrorJson(resp.body); }
    LOG(LogChannel::NET, "getRoomKeysVersions: ok=%d http=%d body=%.200s",
        r.ok ? 1 : 0, r.httpStatus, resp.body.c_str());
    return r;
}

ApiResult<std::string> MatrixClient::getRoomKeys(const std::string& version) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpGet(account().homeserverUrl + "/_matrix/client/v3/room_keys/keys?version="
                        + urlEncodePath(version), authHeaders(), 30000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { r.error = progressive::parseMatrixErrorJson(resp.body); }
    LOG(LogChannel::NET, "getRoomKeys: ok=%d http=%d body=%.300s",
        r.ok ? 1 : 0, r.httpStatus, resp.body.c_str());
    return r;
}

ApiResult<bool> MatrixClient::deleteRoomKeysVersion(const std::string& version) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpDelete(account().homeserverUrl + "/_matrix/client/v3/room_keys/version/"
                           + urlEncodePath(version), authHeaders(), 15000);
    r.httpStatus = resp.statusCode; r.ok = resp.success; r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<std::string> MatrixClient::queryKeys(const std::string& body) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/keys/query",
                         body, authHeaders(), 30000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
           r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage; }
    return r;
}

ApiResult<std::string> MatrixClient::claimKeys(const std::string& body) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    auto resp = httpPost(account().homeserverUrl + "/_matrix/client/v3/keys/claim",
                         body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
           r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage; }
    return r;
}

ApiResult<bool> MatrixClient::sendToDevice(const std::string& eventType,
                                              const std::string& txnId,
                                              const std::string& body) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string url = account().homeserverUrl + "/_matrix/client/v3/sendToDevice/" +
                      eventType + "/" + txnId;
    auto resp = httpPut(url, body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    r.ok = resp.success;
    r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

// ---- Media upload ----

ApiResult<std::string> MatrixClient::uploadMedia(const std::vector<uint8_t>& data,
                                                     const std::string& filename,
                                                     const std::string& contentType) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/media/v3/upload";
    if (!filename.empty()) url << "?filename=" << percentEncode(filename);

    // Build headers with content type
    auto hdrs = authHeaders();
    hdrs["Content-Type"] = contentType.empty() ? "application/octet-stream" : contentType;

    HttpRequest req;
    req.method = "POST";
    req.url = url.str();
    req.body.assign(reinterpret_cast<const char*>(data.data()), data.size());
    req.headers = hdrs;
    req.timeoutMs = 60000;
    req.followRedirects = true;
    auto resp = httpExecute(req);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.ok = true;
        r.data = progressive::parseJsonStringValue(resp.body, "content_uri");
        if (r.data.empty()) r.error.message = "upload: no content_uri in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    LOG(LogChannel::NET, "uploadMedia: status=%d ok=%d mxc=%.120s err=%.200s",
        r.httpStatus, r.ok ? 1 : 0, r.data.c_str(), r.error.message.c_str());
    return r;
}

// ---- Registration ----

ApiResult<AccountInfo> MatrixClient::registerAccount(const std::string& username,
                                                        const std::string& password,
                                                        const std::string& homeserverUrl,
                                                        const std::string& regToken) {
    ApiResult<AccountInfo> r;
    std::ostringstream body;
    if (!regToken.empty()) {
        body << R"({"username":")" << jsonEscape(username) << R"(","password":")"
             << jsonEscape(password) << R"(","auth":{"type":"m.login.registration_token",)"
             << R"("token":")" << jsonEscape(regToken) << R"("}})";
    } else {
        body << R"({"username":")" << jsonEscape(username) << R"(","password":")"
             << jsonEscape(password) << R"(","auth":{"type":"m.login.dummy"}})";
    }

    auto hdrs = std::unordered_map<std::string, std::string>{
        {"Content-Type", "application/json"}
    };
    auto resp = httpPost(homeserverUrl + "/_matrix/client/v3/register?kind=user",
                         body.str(), hdrs, 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        // Parse the response: {"user_id":"...","device_id":"...","access_token":"..."}
        AccountInfo acct;
        acct.userId = progressive::parseJsonStringValue(resp.body, "user_id");
        acct.deviceId = progressive::parseJsonStringValue(resp.body, "device_id");
        acct.accessToken = progressive::parseJsonStringValue(resp.body, "access_token");
        acct.homeserverUrl = homeserverUrl;
        if (!acct.userId.empty() && !acct.accessToken.empty()) {
            r.ok = true;
            r.data = acct;
            return r;
        }
        r.error.message = "register: missing user_id or access_token in response";
        return r;
    }
    // 401 means the server requires additional auth stages (e.g. captcha)
    if (resp.statusCode == 401) {
        // Check if m.login.dummy is in the flows — if so, retry with session
        auto session = progressive::parseJsonStringValue(resp.body, "session");
        if (!session.empty()) {
            // Retry with the session
            std::ostringstream body2;
            body2 << R"({"username":")" << jsonEscape(username) << R"(","password":")"
                  << jsonEscape(password) << R"(","auth":{"type":")"
                  << (regToken.empty() ? "m.login.dummy" : "m.login.registration_token")
                  << R"(","session":")" << jsonEscape(session) << R"(")";
            if (!regToken.empty()) {
                body2 << R"(,"token":")" << jsonEscape(regToken) << R"(")";
            }
            body2 << R"(}})";
            auto resp2 = httpPost(homeserverUrl + "/_matrix/client/v3/register?kind=user",
                                  body2.str(), hdrs, 15000);
            if (resp2.success) {
                AccountInfo acct;
                acct.userId = progressive::parseJsonStringValue(resp2.body, "user_id");
                acct.deviceId = progressive::parseJsonStringValue(resp2.body, "device_id");
                acct.accessToken = progressive::parseJsonStringValue(resp2.body, "access_token");
                acct.homeserverUrl = homeserverUrl;
                if (!acct.userId.empty() && !acct.accessToken.empty()) {
                    r.ok = true;
                    r.data = acct;
                    return r;
                }
            }
        }
        r.error.code = "M_NEEDS_CAPTCHA";
        r.error.message = "This server requires captcha for registration. "
                         "Please register via browser (app.element.io/#/register).";
        return r;
    }
    // 403 with registration token error
    if ((resp.statusCode == 403 || resp.statusCode == 401)
        && !regToken.empty()) {
        auto errBody = progressive::parseMatrixErrorJson(resp.body);
        if (errBody.message.find("registration_token") != std::string::npos) {
            r.error.code = "M_REGISTRATION_TOKEN_INVALID";
            r.error.message = "Invalid or expired registration token.";
            return r;
        }
    }
    // Other error
    if (!resp.body.empty()) {
        r.error = progressive::parseMatrixErrorJson(resp.body);
    } else {
        r.error.message = resp.errorMessage.empty() ? "registration failed" : resp.errorMessage;
    }
    return r;
}

// ---- Profile ----

ApiResult<bool> MatrixClient::setDisplayName(const std::string& displayName) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string body = "{\"displayname\":\"" + jsonEscape(displayName) + "\"}";
    std::string url = account().homeserverUrl + "/_matrix/client/v3/profile/" +
                     urlEncodePath(account().userId) + "/displayname";
    auto resp = httpPut(url, body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    r.ok = resp.success;
    r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<bool> MatrixClient::setAvatarUrl(const std::string& mxcUrl) {
    ApiResult<bool> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string body = "{\"avatar_url\":\"" + jsonEscape(mxcUrl) + "\"}";
    std::string url = account().homeserverUrl + "/_matrix/client/v3/profile/" +
                     urlEncodePath(account().userId) + "/avatar_url";
    auto resp = httpPut(url, body, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    r.ok = resp.success;
    r.data = resp.success;
    if (!resp.success && !resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
    return r;
}

ApiResult<std::string> MatrixClient::getProfile(const std::string& userId) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string url = account().homeserverUrl + "/_matrix/client/v3/profile/" + urlEncodePath(userId);
    auto resp = httpGet(url, authHeaders(), 10000);
    r.httpStatus = resp.statusCode;
    if (resp.success) { r.ok = true; r.data = resp.body; }
    else { if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
           r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage; }
    return r;
}

ApiResult<std::string> MatrixClient::sendThreadReply(const std::string& roomId,
                                                        const std::string& body,
                                                        const std::string& rootEventId,
                                                        const std::string& msgtype) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string txn = genTxnId("pd");
    std::ostringstream jsonBody;
    jsonBody << R"({"msgtype":")" << msgtype << R"(","body":")" << jsonEscape(body)
             << R"(","m.relates_to":{"rel_type":"m.thread","event_id":")"
             << jsonEscape(rootEventId) << R"("}})";
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/"
        << urlEncodePath(roomId) << "/send/m.room.message/" << txn;
    auto resp = httpPut(url.str(), jsonBody.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "event_id");
        r.ok = !r.data.empty();
        if (!r.ok) r.error.message = "send: no event_id in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::sendEncryptedEvent(const std::string& roomId,
                                                            const std::string& contentJson,
                                                            const std::string& txnId) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/"
        << urlEncodePath(roomId) << "/send/m.room.encrypted/" << txnId;
    auto resp = httpPut(url.str(), contentJson, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "event_id");
        r.ok = !r.data.empty();
        if (!r.ok) r.error.message = "send: no event_id in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::sendMessageEvent(const std::string& roomId,
                                                          const std::string& eventType,
                                                          const std::string& contentJson) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string txn = genTxnId("pd");
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/"
        << urlEncodePath(roomId) << "/send/" << eventType << "/" << txn;
    auto resp = httpPut(url.str(), contentJson, authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "event_id");
        r.ok = !r.data.empty();
        if (!r.ok) r.error.message = "send: no event_id in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

ApiResult<std::string> MatrixClient::editMessage(const std::string& roomId,
                                                     const std::string& originalEventId,
                                                     const std::string& newBody) {
    ApiResult<std::string> r;
    if (!isLoggedIn()) { r.error.message = "not logged in"; return r; }
    std::string txn = genTxnId("edit");
    std::ostringstream jsonBody;
    jsonBody << R"({"msgtype":"m.text","body":")" << jsonEscape(newBody)
             << R"(","m.new_content":{"msgtype":"m.text","body":")" << jsonEscape(newBody)
             << R"("},"m.relates_to":{"rel_type":"m.replace","event_id":")"
             << jsonEscape(originalEventId) << R"("}})";
    std::ostringstream url;
    url << account().homeserverUrl << "/_matrix/client/v3/rooms/"
        << urlEncodePath(roomId) << "/send/m.room.message/" << txn;
    auto resp = httpPut(url.str(), jsonBody.str(), authHeaders(), 15000);
    r.httpStatus = resp.statusCode;
    if (resp.success) {
        r.data = progressive::parseJsonStringValue(resp.body, "event_id");
        r.ok = !r.data.empty();
        if (!r.ok) r.error.message = "edit: no event_id in response";
    } else {
        if (!resp.body.empty()) r.error = progressive::parseMatrixErrorJson(resp.body);
        r.error.message = resp.errorMessage.empty() ? r.error.message : resp.errorMessage;
    }
    return r;
}

} // namespace progressive::desktop

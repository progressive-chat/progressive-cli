#include "client.hpp"
#include "error.hpp"
#include "pushrules.hpp"
#include "../database/db.hpp"
#include "../e2ee/crypto.hpp"
#include "../util/logger.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cstdio>

using json = nlohmann::json;

namespace matrixcli { namespace matrix {

struct Client::Impl {
    http::Client http;
    Credentials creds;
    std::string homeserver_url;
    std::string next_batch;
    std::atomic<bool> logged_in{false};
    std::atomic<bool> syncing{false};
    std::atomic<int> timeout{30};
    std::thread sync_thread;
    db::Database* db = nullptr;
    std::unique_ptr<e2ee::CryptoManager> crypto;
    std::map<std::string, bool> encrypted_rooms;
    PushRules pushRules;
    std::map<std::string, std::string> directChats;
    std::map<std::string, std::vector<std::string>> spaceChildren; // space_id -> [room_ids]
    std::map<std::string, std::string> spaceParents; // room_id -> space_id
    bool pushRulesLoaded = false;
};

Client::Client() : impl(std::make_unique<Impl>()) {}
Client::~Client() { stopSync(); }

void Client::setHomeserverURL(const std::string& url) {
    impl->homeserver_url = url;
    if (!impl->homeserver_url.empty() && impl->homeserver_url.back() == '/')
        impl->homeserver_url.pop_back();
}

std::string Client::homeserverURL() const { return impl->homeserver_url; }

void Client::setProxy(const http::ProxyConfig& config) {
    impl->http.setProxy(config);
}

void Client::setTimeout(int seconds) {
    impl->timeout = seconds;
    impl->http.setTimeout(seconds);
}

void Client::setAccessToken(const std::string& token) {
    impl->creds.access_token = token;
}

void Client::setDatabase(db::Database* db) {
    impl->db = db;
}

bool Client::initCrypto(const std::string& userId, const std::string& deviceId) {
    try {
        impl->crypto = std::make_unique<e2ee::CryptoManager>();
        impl->crypto->initAccount(userId, deviceId);
        util::Logger::instance().info("Crypto initialized for " + userId + " / " + deviceId);
        return true;
    } catch (const std::exception& e) {
        util::Logger::instance().error(std::string("Crypto init failed: ") + e.what());
        return false;
    }
}

bool Client::enableEncryption(const std::string& roomId) {
    impl->encrypted_rooms[roomId] = true;
    if (impl->crypto) {
        try {
            impl->crypto->startMegolmOutbound(roomId);
        } catch (...) {}
    }
    return true;
}

bool Client::isRoomEncrypted(const std::string& roomId) const {
    auto it = impl->encrypted_rooms.find(roomId);
    return it != impl->encrypted_rooms.end() && it->second;
}

void Client::loadPushRules() {
    try {
        auto rules = getPushRules();
        impl->pushRules.load(rules.dump());
        impl->pushRulesLoaded = true;
    } catch (...) {
        impl->pushRulesLoaded = true; // Use defaults
    }
}

PushResult Client::evaluatePush(const json& event) {
    if (!impl->pushRulesLoaded) loadPushRules();
    return impl->pushRules.evaluate(event);
}

void Client::loadDirectChats() {
    try {
        auto resp = authGet("/_matrix/client/r0/user/" +
            http::urlEncode(impl->creds.user_id) + "/account_data/m.direct");
        if (!resp.ok()) return;
        auto j = json::parse(resp.body);
        impl->directChats.clear();
        for (auto& [userId, rooms] : j.items()) {
            if (rooms.is_array() && !rooms.empty()) {
                std::string roomId = rooms[0];
                impl->directChats[roomId] = userId;
            }
        }
    } catch (...) {}
}

bool Client::isDirectChat(const std::string& room_id) const {
    return impl->directChats.count(room_id) > 0;
}

std::string Client::dmUserId(const std::string& room_id) const {
    auto it = impl->directChats.find(room_id);
    return it != impl->directChats.end() ? it->second : "";
}

std::vector<std::string> Client::getSpaceChildren(const std::string& space_id) const {
    auto it = impl->spaceChildren.find(space_id);
    return it != impl->spaceChildren.end() ? it->second : std::vector<std::string>{};
}

bool Client::isSpaceRoom(const std::string& room_id) const {
    return impl->spaceChildren.count(room_id) > 0;
}

std::string Client::getSSOLoginURL(const std::string& redirect_uri) {
    auto flows = getLoginFlows();
    bool has_sso = false;
    for (auto& flow : flows.flows) {
        if (flow.type == "m.login.sso" || flow.type == "m.login.cas") {
            has_sso = true;
            break;
        }
    }
    if (!has_sso) return "";

    std::string hs = impl->homeserver_url;
    if (hs.back() == '/') hs.pop_back();
    std::string url = hs + "/_matrix/client/r0/login/sso/redirect";
    if (!redirect_uri.empty()) url += "?redirectUrl=" + http::urlEncode(redirect_uri);
    return url;
}

bool Client::sendTyping(const std::string& room_id, bool typing, int timeout_ms) {
    json body = {
        {"typing", typing},
        {"timeout", timeout_ms}
    };
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id + "/typing/" +
        http::urlEncode(impl->creds.user_id), body.dump());
    return resp.ok();
}

std::string Client::sendPoll(const std::string& room_id, const std::string& question,
                              const std::vector<std::string>& answers) {
    json pollAnswers = json::array();
    for (auto& a : answers)
        pollAnswers.push_back({{"id", a}, {"m.text", {{"body", a}}}});

    json content = {
        {"m.poll", {
            {"question", {
                {"m.text", {{"body", question}}}
            }},
            {"answers", pollAnswers},
            {"max_selections", 1},
            {"kind", "m.poll.disclosed"}
        }},
        {"m.text", {
            {"body", question + "\n" + 
                [&](){ std::string s; for (size_t i = 0; i < answers.size(); i++) 
                    s += std::to_string(i+1) + ". " + answers[i] + "\n"; return s; }()}
        }}
    };

    std::string txn = generateTxnId();
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id +
                         "/send/m.poll.start/" + txn, content.dump());
    checkResponse(resp);
    return json::parse(resp.body)["event_id"].get<std::string>();
}

std::string Client::sendPollResponse(const std::string& room_id,
                                      const std::string& poll_event_id,
                                      const std::vector<std::string>& answer_ids) {
    json rel = {{"event_id", poll_event_id}, {"rel_type", "m.reference"}};
    json content = {
        {"m.relates_to", rel},
        {"m.poll.response", {
            {"answers", answer_ids}
        }}
    };

    std::string txn = generateTxnId();
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id +
                         "/send/m.poll.response/" + txn, content.dump());
    checkResponse(resp);
    return json::parse(resp.body)["event_id"].get<std::string>();
}

std::string Client::sendReaction(const std::string& room_id, const std::string& event_id,
                                  const std::string& key) {
    json content = {
        {"m.relates_to", {
            {"event_id", event_id},
            {"rel_type", "m.annotation"},
            {"key", key}
        }}
    };
    std::string txn = generateTxnId();
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id +
                         "/send/m.reaction/" + txn, content.dump());
    checkResponse(resp);
    return json::parse(resp.body)["event_id"].get<std::string>();
}

std::string Client::sendVoiceMessage(const std::string& room_id, const std::string& mxc_url,
                                      int64_t duration_ms, const std::string& mimetype) {
    json content = {
        {"msgtype", "m.audio"},
        {"body", "Voice message"},
        {"url", mxc_url},
        {"info", {
            {"duration", duration_ms},
            {"mimetype", mimetype}
        }}
    };
    std::string txn = generateTxnId();
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id +
                         "/send/m.room.message/" + txn, content.dump());
    checkResponse(resp);
    return json::parse(resp.body)["event_id"].get<std::string>();
}

std::string Client::sendSticker(const std::string& room_id, const std::string& mxc_url,
                                 const std::string& body, const std::string& mimetype) {
    json content = {
        {"body", body},
        {"url", mxc_url},
        {"info", {{"mimetype", mimetype}}}
    };
    std::string txn = generateTxnId();
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id +
                         "/send/m.sticker/" + txn, content.dump());
    checkResponse(resp);
    return json::parse(resp.body)["event_id"].get<std::string>();
}

std::string Client::sendLocation(const std::string& room_id, const std::string& geo_uri,
                                  const std::string& description) {
    json content = {
        {"msgtype", "m.location"},
        {"body", description.empty() ? geo_uri : description},
        {"geo_uri", geo_uri},
        {"org.matrix.msc3488.location", {
            {"uri", geo_uri}
        }}
    };
    if (!description.empty()) {
        content["org.matrix.msc3488.location"]["description"] = description;
    }
    std::string txn = generateTxnId();
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id +
                         "/send/m.room.message/" + txn, content.dump());
    checkResponse(resp);
    return json::parse(resp.body)["event_id"].get<std::string>();
}

std::string Client::sendTodo(const std::string& room_id, const std::string& title,
                              const json& items) {
    json content = {
        {"title", title},
        {"items", items}
    };
    std::string txn = generateTxnId();
    auto resp = authPut("/_matrix/client/r0/rooms/" + room_id +
                         "/send/im.vector.todo/" + txn, content.dump());
    if (resp.status_code != 200) {
        // Fallback to state event
        return sendStateEvent(room_id, "im.vector.todo", "", content);
    }
    checkResponse(resp);
    return json::parse(resp.body)["event_id"].get<std::string>();
}

json Client::getURLPreview(const std::string& url) {
    auto resp = authGet("/_matrix/media/r0/preview_url?url=" + http::urlEncode(url));
    if (resp.ok()) return json::parse(resp.body);
    return json::object();
}

bool Client::loadCrossSigningKeys() {
    if (!impl->crypto) return false;
    try {
        auto resp = authGet("/_matrix/client/r0/keys/query");
        if (!resp.ok()) return false;
        auto j = json::parse(resp.body);
        if (j.contains("master_keys") && j["master_keys"].contains(impl->creds.user_id)) {
            auto& mk = j["master_keys"][impl->creds.user_id];
            auto ssk = j.value("self_signing_keys", json::object());
            auto usk = j.value("user_signing_keys", json::object());
            impl->crypto->setCrossSigningKeys(
                mk.dump(),
                ssk.value(impl->creds.user_id, json::object()).dump(),
                usk.value(impl->creds.user_id, json::object()).dump()
            );
            return true;
        }
    } catch (...) {}
    return false;
}

bool Client::uploadDeviceKeys() {
    if (!impl->crypto) return false;
    try {
        auto dk = impl->crypto->deviceKeys();
        json body;
        body["device_keys"] = {
            {"user_id", dk.userId},
            {"device_id", dk.deviceId},
            {"algorithms", {"m.olm.v1.curve25519-aes-sha2", "m.megolm.v1.aes-sha2"}},
            {"keys", {
                {"ed25519:" + dk.deviceId, dk.ed25519Key},
                {"curve25519:" + dk.deviceId, dk.curve25519Key}
            }},
            {"signatures", {{dk.userId, {{"ed25519:" + dk.deviceId, impl->crypto->signMessage("")}}}}}
        };

        auto resp = authPost("/_matrix/client/r0/keys/upload", body.dump());
        return resp.ok();
    } catch (...) { return false; }
}

bool Client::uploadKeyBackup() {
    if (!impl->crypto) return false;
    try {
        // Get or create backup version
        auto resp = authPost("/_matrix/client/r0/room_keys/version", R"({"algorithm":"m.megolm_backup.v1.curve25519-aes-sha2"})");
        if (!resp.ok() && resp.status_code != 409) return false;

        // Upload all rooms' keys
        json rooms;
        for (auto& [id, _] : impl->encrypted_rooms) {
            auto keys = impl->crypto->exportRoomKeys(id);
            auto kj = json::parse(keys);
            for (auto& [rid, data] : kj.value("rooms", json::object()).items()) {
                rooms[rid] = data;
            }
        }

        json body;
        body["rooms"] = rooms;
        auto putResp = authPut("/_matrix/client/r0/room_keys/keys", body.dump());
        return putResp.ok();
    } catch (...) { return false; }
}

bool Client::restoreKeyBackup() {
    try {
        auto resp = authGet("/_matrix/client/r0/room_keys/keys");
        if (!resp.ok()) return false;
        auto j = json::parse(resp.body);
        if (j.contains("rooms") && impl->crypto) {
            for (auto& [roomId, roomData] : j["rooms"].items()) {
                impl->crypto->importRoomKeys(roomId, roomData.dump());
            }
            return true;
        }
    } catch (...) {}
    return false;
}

std::string Client::buildUrl(const std::string& path) const {
    return impl->homeserver_url + path;
}

std::string Client::buildUrl(
    const std::string& path,
    const std::map<std::string, std::string>& params) const
{
    std::string url = impl->homeserver_url + path;
    bool first = true;
    for (const auto& [k, v] : params) {
        url += (first ? "?" : "&");
        url += http::urlEncode(k) + "=" + http::urlEncode(v);
        first = false;
    }
    return url;
}

void Client::checkResponse(const http::Response& resp) {
    if (!resp.ok()) {
        throw MatrixException(makeMatrixError(resp));
    }
}

MatrixError Client::makeMatrixError(const http::Response& resp) {
    return parseMatrixError(resp.body, resp.status_code);
}

http::Response Client::authGet(const std::string& path, int timeout) {
    std::string url = impl->homeserver_url + path;
    std::map<std::string, std::string> headers;
    if (!impl->creds.access_token.empty())
        headers["Authorization"] = "Bearer " + impl->creds.access_token;
    return impl->http.get(url, headers);
}

http::Response Client::authGet(
    const std::string& path,
    const std::map<std::string, std::string>& params,
    int timeout)
{
    std::string full_path = path;
    bool first = true;
    for (const auto& [k, v] : params) {
        full_path += (first ? "?" : "&");
        full_path += http::urlEncode(k) + "=" + http::urlEncode(v);
        first = false;
    }
    return authGet(full_path, timeout);
}

http::Response Client::authPost(const std::string& path,
                                const std::string& json_body, int timeout) {
    std::string url = impl->homeserver_url + path;
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    if (!impl->creds.access_token.empty())
        headers["Authorization"] = "Bearer " + impl->creds.access_token;
    return impl->http.post(url, json_body, headers);
}

http::Response Client::authPut(const std::string& path,
                               const std::string& json_body, int timeout) {
    std::string url = impl->homeserver_url + path;
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    if (!impl->creds.access_token.empty())
        headers["Authorization"] = "Bearer " + impl->creds.access_token;
    return impl->http.put(url, json_body, headers);
}

http::Response Client::authDelete(const std::string& path, int timeout) {
    std::string url = impl->homeserver_url + path;
    std::map<std::string, std::string> headers;
    if (!impl->creds.access_token.empty())
        headers["Authorization"] = "Bearer " + impl->creds.access_token;
    return impl->http.del(url, headers);
}

ServerVersions Client::getServerVersions() {
    auto resp = impl->http.get(buildUrl("/_matrix/client/versions"));
    if (!resp.ok()) return {};
    return ServerVersions::fromJson(json::parse(resp.body));
}

WellKnown Client::getWellKnown(const std::string& server_name) {
    std::string url = "https://" + server_name + "/.well-known/matrix/client";
    auto resp = impl->http.get(url);
    if (!resp.ok()) return {};
    return WellKnown::fromJson(json::parse(resp.body));
}

LoginFlowsResult Client::getLoginFlows() {
    auto resp = impl->http.get(buildUrl("/_matrix/client/r0/login"));
    if (resp.status_code != 200) {
        throw std::runtime_error("Failed to get login flows: HTTP " +
                                std::to_string(resp.status_code));
    }
    return parseLoginFlows(resp.body);
}

Credentials Client::loginPassword(const std::string& username,
                                   const std::string& password,
                                   const std::string& device_name) {
    json req = {
        {"type", "m.login.password"},
        {"identifier", {{"type", "m.id.user"}, {"user", username}}},
        {"password", password}
    };
    if (!device_name.empty())
        req["initial_device_display_name"] = device_name;

    auto resp = impl->http.post(
        buildUrl("/_matrix/client/r0/login"),
        req.dump(),
        {{"Content-Type", "application/json"}}
    );

    if (resp.status_code != 200) {
        auto err = makeMatrixError(resp);
        throw MatrixException(err);
    }

    auto j = json::parse(resp.body);
    impl->creds.user_id = j["user_id"].get<std::string>();
    impl->creds.access_token = j["access_token"].get<std::string>();
    impl->creds.device_id = j.value("device_id", "");
    impl->creds.refresh_token = j.value("refresh_token", "");
    impl->creds.homeserver_url = impl->homeserver_url;
    impl->logged_in = true;

    return impl->creds;
}

Credentials Client::loginToken(const std::string& token,
                                const std::string& device_name) {
    json req = {
        {"type", "m.login.token"},
        {"token", token}
    };
    if (!device_name.empty())
        req["initial_device_display_name"] = device_name;

    auto resp = impl->http.post(
        buildUrl("/_matrix/client/r0/login"),
        req.dump(),
        {{"Content-Type", "application/json"}}
    );

    if (resp.status_code != 200) {
        auto err = makeMatrixError(resp);
        throw MatrixException(err);
    }

    auto j = json::parse(resp.body);
    impl->creds.user_id = j["user_id"].get<std::string>();
    impl->creds.access_token = j["access_token"].get<std::string>();
    impl->creds.device_id = j.value("device_id", "");
    impl->creds.refresh_token = j.value("refresh_token", "");
    impl->creds.homeserver_url = impl->homeserver_url;
    impl->logged_in = true;

    return impl->creds;
}

Credentials Client::loginSSO(const std::string& token,
                              const std::string& device_name) {
    return loginToken(token, device_name);
}

Credentials Client::registerAccount(const std::string& username,
                                     const std::string& password,
                                     const std::string& device_name) {
    json req = {
        {"username", username},
        {"password", password},
        {"type", "m.login.password"}
    };
    if (!device_name.empty())
        req["initial_device_display_name"] = device_name;

    auto resp = impl->http.post(
        buildUrl("/_matrix/client/r0/register"),
        req.dump(),
        {{"Content-Type", "application/json"}}
    );

    if (resp.status_code == 401) {
        // Interactive auth: try with dummy auth
        req["auth"] = {{"type", "m.login.dummy"}};
        resp = impl->http.post(
            buildUrl("/_matrix/client/r0/register"),
            req.dump(),
            {{"Content-Type", "application/json"}}
        );
    }

    if (resp.status_code != 200) {
        auto err = makeMatrixError(resp);
        throw MatrixException(err);
    }

    auto j = json::parse(resp.body);
    impl->creds.user_id = j["user_id"].get<std::string>();
    impl->creds.access_token = j["access_token"].get<std::string>();
    impl->creds.device_id = j.value("device_id", "");
    impl->logged_in = true;
    return impl->creds;
}

SessionInfo Client::whoAmI() {
    auto resp = authGet("/_matrix/client/r0/account/whoami");
    checkResponse(resp);
    return SessionInfo::fromJson(json::parse(resp.body));
}

bool Client::logout() {
    auto resp = authPost("/_matrix/client/r0/logout", "{}");
    if (resp.ok()) {
        impl->logged_in = false;
        impl->creds = Credentials{};
    }
    return resp.ok();
}

bool Client::logoutAll() {
    auto resp = authPost("/_matrix/client/r0/logout/all", "{}");
    return resp.ok();
}

std::string Client::generateTxnId() const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return std::to_string(ms) + "_" + std::to_string(rand());
}

std::string Client::sendMessage(const std::string& room_id,
                                 const std::string& body,
                                 const std::string& msgtype) {
    json content = {
        {"msgtype", msgtype},
        {"body", body}
    };

    std::string event_type = "m.room.message";

    // Encrypt if room is encrypted and crypto is available
    if (isRoomEncrypted(room_id) && impl->crypto) {
        try {
            std::string sessionId, ciphertext;
            impl->crypto->encryptMegolm(room_id, content.dump(), sessionId, ciphertext);
            auto dk = impl->crypto->deviceKeys();
            content = {
                {"algorithm", "m.megolm.v1.aes-sha2"},
                {"sender_key", dk.curve25519Key},
                {"session_id", sessionId},
                {"ciphertext", ciphertext},
                {"device_id", dk.deviceId}
            };
            event_type = "m.room.encrypted";
        } catch (const std::exception& e) {
            util::Logger::instance().warn(std::string("Encryption failed, sending unencrypted: ") + e.what());
        }
    }

    std::string txn_id = generateTxnId();
    std::string path = "/_matrix/client/r0/rooms/" + room_id +
                       "/send/" + event_type + "/" + txn_id;

    auto resp = authPut(path, content.dump());
    checkResponse(resp);

    auto j = json::parse(resp.body);
    return j["event_id"].get<std::string>();
}

std::string Client::sendTextMessage(const std::string& room_id,
                                     const std::string& body) {
    return sendMessage(room_id, body, "m.text");
}

std::string Client::sendThreadReply(const std::string& room_id,
                                     const std::string& thread_root,
                                     const std::string& body) {
    json content = {
        {"msgtype", "m.text"},
        {"body", body},
        {"m.relates_to", {
            {"event_id", thread_root},
            {"rel_type", "m.thread"}
        }}
    };

    std::string txn_id = generateTxnId();
    std::string path = "/_matrix/client/r0/rooms/" + room_id + "/send/m.room.message/" + txn_id;
    auto resp = authPut(path, content.dump());
    checkResponse(resp);

    return json::parse(resp.body)["event_id"].get<std::string>();
}

std::string Client::sendNotice(const std::string& room_id,
                                const std::string& body) {
    return sendMessage(room_id, body, "m.notice");
}

std::string Client::sendEmote(const std::string& room_id,
                               const std::string& body) {
    return sendMessage(room_id, body, "m.emote");
}

std::string Client::sendImageMessage(const std::string& room_id,
                                      const std::string& mxc_url,
                                      const std::string& filename,
                                      int64_t size,
                                      const std::string& mimetype,
                                      int width, int height) {
    json info = {
        {"mimetype", mimetype},
        {"size", size},
        {"w", width},
        {"h", height}
    };
    json content = {
        {"msgtype", "m.image"},
        {"body", filename},
        {"url", mxc_url},
        {"info", info}
    };
    return sendEvent(room_id, "m.room.message", content);
}

std::string Client::sendFileMessage(const std::string& room_id,
                                     const std::string& mxc_url,
                                     const std::string& filename,
                                     int64_t size,
                                     const std::string& mimetype) {
    json info = {
        {"mimetype", mimetype},
        {"size", size}
    };
    json content = {
        {"msgtype", "m.file"},
        {"body", filename},
        {"url", mxc_url},
        {"info", info}
    };
    return sendEvent(room_id, "m.room.message", content);
}

std::string Client::sendEvent(const std::string& room_id,
                               const std::string& event_type,
                               const json& content) {
    std::string txn_id = generateTxnId();
    std::string path = "/_matrix/client/r0/rooms/" + room_id +
                       "/send/" + event_type + "/" + txn_id;

    auto resp = authPut(path, content.dump());
    checkResponse(resp);

    auto j = json::parse(resp.body);
    return j["event_id"].get<std::string>();
}

std::string Client::sendStateEvent(const std::string& room_id,
                                    const std::string& event_type,
                                    const std::string& state_key,
                                    const json& content) {
    std::string path = "/_matrix/client/r0/rooms/" + room_id +
                       "/state/" + event_type + "/" + state_key;

    auto resp = authPut(path, content.dump());
    checkResponse(resp);

    auto j = json::parse(resp.body);
    return j["event_id"].get<std::string>();
}

std::string Client::redactEvent(const std::string& room_id,
                                 const std::string& event_id,
                                 const std::string& reason) {
    std::string txn_id = generateTxnId();
    std::string path = "/_matrix/client/r0/rooms/" + room_id +
                       "/redact/" + event_id + "/" + txn_id;

    json body;
    if (!reason.empty()) body["reason"] = reason;

    auto resp = authPut(path, body.dump());
    checkResponse(resp);

    auto j = json::parse(resp.body);
    return j["event_id"].get<std::string>();
}

std::string Client::createRoom(const std::string& name,
                                const std::string& topic,
                                bool is_direct,
                                const std::vector<std::string>& invite_users) {
    json body = json::object();
    if (!name.empty()) body["name"] = name;
    if (!topic.empty()) body["topic"] = topic;
    if (is_direct) body["is_direct"] = true;
    if (!invite_users.empty()) {
        body["invite"] = invite_users;
    }
    body["preset"] = is_direct ? "trusted_private_chat" : "private_chat";

    auto resp = authPost("/_matrix/client/r0/createRoom", body.dump());
    checkResponse(resp);

    auto j = json::parse(resp.body);
    return j["room_id"].get<std::string>();
}

bool Client::joinRoom(const std::string& room_id, const std::string& reason) {
    json body;
    if (!reason.empty()) body["reason"] = reason;
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/join", body.dump());
    return resp.ok();
}

bool Client::leaveRoom(const std::string& room_id) {
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/leave", "{}");
    return resp.ok();
}

bool Client::inviteUser(const std::string& room_id, const std::string& user_id,
                         const std::string& reason) {
    json body = {{"user_id", user_id}};
    if (!reason.empty()) body["reason"] = reason;
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/invite", body.dump());
    return resp.ok();
}

bool Client::kickUser(const std::string& room_id, const std::string& user_id,
                       const std::string& reason) {
    json body = {{"user_id", user_id}};
    if (!reason.empty()) body["reason"] = reason;
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/kick", body.dump());
    return resp.ok();
}

bool Client::banUser(const std::string& room_id, const std::string& user_id,
                      const std::string& reason) {
    json body = {{"user_id", user_id}};
    if (!reason.empty()) body["reason"] = reason;
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/ban", body.dump());
    return resp.ok();
}

bool Client::unbanUser(const std::string& room_id, const std::string& user_id) {
    json body = {{"user_id", user_id}};
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/unban", body.dump());
    return resp.ok();
}

std::string Client::setRoomName(const std::string& room_id, const std::string& name) {
    json content = {{"name", name}};
    return sendStateEvent(room_id, "m.room.name", "", content);
}

std::string Client::setRoomTopic(const std::string& room_id, const std::string& topic) {
    json content = {{"topic", topic}};
    return sendStateEvent(room_id, "m.room.topic", "", content);
}

bool Client::setRoomAvatar(const std::string& room_id, const std::string& mxc_url) {
    json content = {{"url", mxc_url}};
    sendStateEvent(room_id, "m.room.avatar", "", content);
    return true;
}

std::vector<Event> Client::getRoomState(const std::string& room_id) {
    auto resp = authGet("/_matrix/client/r0/rooms/" + room_id + "/state");
    checkResponse(resp);
    auto j = json::parse(resp.body);
    std::vector<Event> events;
    for (auto& ev : j)
        events.push_back(Event::fromJson(ev));
    return events;
}

std::vector<Event> Client::getRoomMembers(const std::string& room_id) {
    auto resp = authGet("/_matrix/client/r0/rooms/" + room_id + "/members");
    checkResponse(resp);
    auto j = json::parse(resp.body);
    std::vector<Event> events;
    if (j.contains("chunk")) {
        for (auto& ev : j["chunk"])
            events.push_back(Event::fromJson(ev));
    }
    return events;
}

std::vector<Event> Client::getRoomMessages(const std::string& room_id,
                                            const std::string& from,
                                            const std::string& dir,
                                            int limit) {
    std::map<std::string, std::string> params = {
        {"dir", dir},
        {"limit", std::to_string(limit)}
    };
    if (!from.empty()) params["from"] = from;

    auto resp = authGet("/_matrix/client/r0/rooms/" + room_id + "/messages",
                        params);
    checkResponse(resp);
    auto j = json::parse(resp.body);
    std::vector<Event> events;
    if (j.contains("chunk")) {
        for (auto& ev : j["chunk"])
            events.push_back(Event::fromJson(ev));
    }
    return events;
}

UserInfo Client::getProfile(const std::string& user_id) {
    auto resp = authGet("/_matrix/client/r0/profile/" +
                        http::urlEncode(user_id));
    if (!resp.ok()) return {};
    return UserInfo::fromJson(json::parse(resp.body));
}

std::string Client::getDisplayName(const std::string& user_id) {
    auto resp = authGet("/_matrix/client/r0/profile/" +
                        http::urlEncode(user_id) + "/displayname");
    if (!resp.ok()) return "";
    auto j = json::parse(resp.body);
    return j.value("displayname", "");
}

bool Client::setDisplayName(const std::string& display_name) {
    std::string user_id = impl->creds.user_id;
    json body = {{"displayname", display_name}};
    auto resp = authPut("/_matrix/client/r0/profile/" +
                        http::urlEncode(user_id) + "/displayname",
                        body.dump());
    return resp.ok();
}

std::string Client::getAvatarUrl(const std::string& user_id) {
    auto resp = authGet("/_matrix/client/r0/profile/" +
                        http::urlEncode(user_id) + "/avatar_url");
    if (!resp.ok()) return "";
    auto j = json::parse(resp.body);
    return j.value("avatar_url", "");
}

bool Client::setAvatarUrl(const std::string& avatar_url) {
    std::string user_id = impl->creds.user_id;
    json body = {{"avatar_url", avatar_url}};
    auto resp = authPut("/_matrix/client/r0/profile/" +
                        http::urlEncode(user_id) + "/avatar_url",
                        body.dump());
    return resp.ok();
}

json Client::getDevices() {
    auto resp = authGet("/_matrix/client/r0/devices");
    checkResponse(resp);
    return json::parse(resp.body);
}

bool Client::deleteDevices(const std::vector<std::string>& device_ids) {
    json body = {{"devices", device_ids}};
    auto resp = authPost("/_matrix/client/r0/delete_devices", body.dump());
    return resp.ok();
}

json Client::getPushRules() {
    auto resp = authGet("/_matrix/client/r0/pushrules");
    checkResponse(resp);
    return json::parse(resp.body);
}

std::string Client::createFilter(const std::string& filter_json) {
    auto resp = authPost("/_matrix/client/r0/user/" +
                         http::urlEncode(impl->creds.user_id) + "/filter",
                         filter_json);
    checkResponse(resp);
    auto j = json::parse(resp.body);
    return j["filter_id"].get<std::string>();
}

json Client::getPublicRooms(const std::string& server,
                             const std::string& search_term,
                             int limit) {
    json body = {{"limit", limit}};
    if (!server.empty()) body["server"] = server;
    if (!search_term.empty())
        body["filter"] = {{"generic_search_term", search_term}};

    auto resp = authPost("/_matrix/client/r0/publicRooms", body.dump());
    checkResponse(resp);
    return json::parse(resp.body);
}

json Client::searchMessages(const std::string& search_term,
                             const std::string& room_id,
                             int limit) {
    json search_cat = {
        {"search_term", search_term},
        {"order_by", "recent"},
        {"include_state", false}
    };
    if (!room_id.empty())
        search_cat["filter"] = {{"rooms", {room_id}}};

    json body = {
        {"search_categories", {{"room_events", search_cat}}}
    };

    auto resp = authPost("/_matrix/client/r0/search", body.dump());
    checkResponse(resp);
    return json::parse(resp.body);
}

bool Client::changePassword(const std::string& old_password,
                             const std::string& new_password) {
    json body = {{"new_password", new_password}};
    if (!old_password.empty()) body["old_password"] = old_password;
    auto resp = authPost("/_matrix/client/r0/account/password", body.dump());
    return resp.ok();
}

bool Client::deactivateAccount(const std::string& auth_json) {
    auto resp = authPost("/_matrix/client/r0/account/deactivate", auth_json);
    return resp.ok();
}

bool Client::setPresence(const std::string& presence) {
    json body = {{"presence", presence}};
    auto resp = authPut("/_matrix/client/r0/presence/" +
                        http::urlEncode(impl->creds.user_id) + "/status",
                        body.dump());
    return resp.ok();
}

bool Client::sendReadReceipt(const std::string& room_id, const std::string& event_id) {
    json body;
    if (!event_id.empty()) body = {{"event_id", event_id}};
    // If event_id is empty, mark fully read up to latest
    std::string event = event_id.empty() ?
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) : event_id;
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/read_markers",
                          json{{"m.fully_read", event}, {"m.read", event}}.dump());
    // Fallback for older servers
    if (!resp.ok()) {
        resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/receipt/m.read/" + event,
                         json::object().dump());
    }
    return resp.ok();
}

json Client::getPresence(const std::string& user_id) {
    auto resp = authGet("/_matrix/client/r0/presence/" +
                        http::urlEncode(user_id) + "/status");
    checkResponse(resp);
    return json::parse(resp.body);
}

json Client::getNotifications(const std::string& from, int limit,
                               const std::string& only) {
    std::map<std::string, std::string> params = {{"limit", std::to_string(limit)}};
    if (!from.empty()) params["from"] = from;
    if (!only.empty()) params["only"] = only;

    auto resp = authGet("/_matrix/client/r0/notifications", params);
    checkResponse(resp);
    return json::parse(resp.body);
}

std::string Client::uploadMedia(const std::string& file_path,
                                 const std::string& content_type) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open file: " + file_path);

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string data(static_cast<size_t>(size), '\0');
    file.read(data.data(), size);
    file.close();

    std::string filename = file_path;
    auto slash = filename.find_last_of("/\\");
    if (slash != std::string::npos) filename = filename.substr(slash + 1);

    std::string ct = content_type.empty() ? "application/octet-stream" : content_type;

    std::string boundary = "----FormBoundary" + generateTxnId();

    std::ostringstream body;
    body << "--" << boundary << "\r\n";
    body << "Content-Disposition: form-data; name=\"file\"; filename=\""
         << filename << "\"\r\n";
    body << "Content-Type: " << ct << "\r\n";
    body << "Content-Length: " << size << "\r\n";
    body << "\r\n";
    body.write(data.data(), size);
    body << "\r\n";
    body << "--" << boundary << "--\r\n";

    std::string path = "/_matrix/media/r0/upload?filename=" +
                       http::urlEncode(filename);

    std::string url = impl->homeserver_url + path;
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;
    if (!impl->creds.access_token.empty())
        headers["Authorization"] = "Bearer " + impl->creds.access_token;

    auto resp = impl->http.post(url, body.str(), headers);
    checkResponse(resp);

    auto j = json::parse(resp.body);
    return j["content_uri"].get<std::string>();
}

SyncResponse Client::syncOnce(const std::string& filter,
                               const std::string& since,
                               int timeout_ms) {
    std::map<std::string, std::string> params;
    if (!filter.empty()) params["filter"] = filter;
    if (!since.empty()) params["since"] = since;
    else if (!impl->next_batch.empty()) params["since"] = impl->next_batch;
    params["timeout"] = std::to_string(timeout_ms);

    std::string path = "/_matrix/client/r0/sync";
    auto resp = authGet(path, params, timeout_ms + 10000);

    if (!resp.ok()) {
        auto err = makeMatrixError(resp);
        throw MatrixException(err);
    }

    SyncResponse sync_resp = SyncResponse::fromJson(resp.body);
    impl->next_batch = sync_resp.next_batch;
    return sync_resp;
}

void Client::startSync(EventCallback onEvent, const std::string& filter,
                        int poll_timeout_ms) {
    if (impl->syncing) return;
    impl->syncing = true;

    impl->sync_thread = std::thread([this, onEvent, filter, poll_timeout_ms]() {
        while (impl->syncing) {
            try {
                SyncResponse sr = syncOnce(filter, "", poll_timeout_ms < 0 ? 30000 : poll_timeout_ms);

                // Save next_batch periodically
                if (impl->db && impl->logged_in) {
                    db::StoredAccount acc;
                    acc.homeserver_url = impl->homeserver_url;
                    acc.user_id = impl->creds.user_id;
                    acc.access_token = impl->creds.access_token;
                    acc.device_id = impl->creds.device_id;
                    acc.next_batch = impl->next_batch;
                    impl->db->saveAccount(acc);
                }

                // Handle account_data (m.direct, push rules, etc.)
                for (auto& ev : sr.account_data) {
                    if (ev.type == "m.direct") {
                        impl->directChats.clear();
                        for (auto& [userId, rooms] : ev.content.items()) {
                            if (rooms.is_array() && !rooms.empty()) {
                                impl->directChats[rooms[0].get<std::string>()] = userId;
                            }
                        }
                        util::Logger::instance().info("Loaded " + std::to_string(impl->directChats.size()) + " direct chats");
                    }
                    if (ev.type == "m.push_rules") {
                        try { impl->pushRules.load(ev.content.dump()); impl->pushRulesLoaded = true; } catch (...) {}
                    }
                    onEvent(ev);
                }
                for (auto& ev : sr.presence) onEvent(ev);

                // Handle to-device events (m.room_key, m.room.encrypted Olm)
                for (auto& ev : sr.to_device) {
                    if (impl->crypto && ev.type == "m.room_key" && ev.content.contains("room_id") &&
                        ev.content.contains("session_key") && ev.content.contains("session_id")) {
                        try {
                            std::string src = ev.content.value("sender_key", "");
                            impl->crypto->receiveMegolmSession(
                                ev.content["room_id"].get<std::string>(),
                                src,
                                ev.content["session_key"].get<std::string>());
                            util::Logger::instance().debug("Imported megolm session " +
                                ev.content["session_id"].get<std::string>() + " for room " +
                                ev.content["room_id"].get<std::string>());
                        } catch (const std::exception& e) {
                            util::Logger::instance().warn(std::string("Failed to import megolm session: ") + e.what());
                        }
                    }
                    onEvent(ev);
                }

                for (auto& [room_id, room] : sr.rooms.join) {
                    if (impl->db) impl->db->upsertRoom(room_id, room);

                    // Track encrypted rooms and space hierarchy from state events
                    for (auto& ev : room.state.events) {
                        if (ev.type == "m.room.encryption") {
                            impl->encrypted_rooms[room_id] = true;
                        }
                        if (ev.type == "m.space.child" && ev.content.contains("via")) {
                            impl->spaceChildren[room_id].push_back(ev.state_key);
                            impl->spaceParents[ev.state_key] = room_id;
                        }
                        if (ev.type == "m.room.create" && ev.content.value("type", "") == "m.space") {
                            impl->spaceChildren.emplace(room_id, std::vector<std::string>{});
                        }
                        if (impl->db) impl->db->insertEvent(ev);
                        onEvent(ev);
                    }
                    for (auto& ev : room.timeline.events) {
                        // Decrypt m.room.encrypted events
                        std::string decrypted;
                        if (impl->crypto && ev.type == "m.room.encrypted" && ev.content.contains("ciphertext") &&
                            ev.content.contains("session_id") && ev.content.contains("algorithm") &&
                            ev.content["algorithm"].get<std::string>() == "m.megolm.v1.aes-sha2") {
                            try {
                                uint32_t msgIdx = 0;
                                auto cipher = ev.content["ciphertext"].get<std::string>();
                                auto sessId = ev.content["session_id"].get<std::string>();
                                decrypted = impl->crypto->decryptMegolm(room_id, sessId, cipher, msgIdx);
                                // Parse decrypted JSON back to event content
                                try {
                                    auto dj = json::parse(decrypted);
                                    ev.content = dj;
                                } catch (...) {
                                    ev.content["body"] = decrypted;
                                }
                            } catch (const std::exception& e) {
                                util::Logger::instance().warn("Decrypt failed for " + ev.event_id + ": " + e.what());
                            }
                        }
                        if (impl->db) impl->db->insertEvent(ev, decrypted);
                        onEvent(ev);
                    }
                    for (auto& ev : room.ephemeral.events) onEvent(ev);
                    for (auto& ev : room.account_data.events) onEvent(ev);
                }
                for (auto& [room_id, room] : sr.rooms.invite) {
                    for (auto& ev : room.invite_state) onEvent(ev);
                }
                for (auto& [room_id, room] : sr.rooms.leave) {
                    for (auto& ev : room.timeline.events) onEvent(ev);
                }
            } catch (const std::exception& e) {
                if (impl->syncing) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                }
            }
        }
    });
}

void Client::stopSync() {
    impl->syncing = false;
    if (impl->sync_thread.joinable()) {
        impl->sync_thread.join();
    }
}

std::string Client::nextBatch() const { return impl->next_batch; }

bool Client::isLoggedIn() const { return impl->logged_in; }
std::string Client::userId() const { return impl->creds.user_id; }
Credentials Client::credentials() const { return impl->creds; }

bool Client::ignoreUser(const std::string& user_id) {
    json data;
    auto resp = authGet("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/account_data/m.ignored_user_list");
    if (resp.ok()) {
        try { data = json::parse(resp.body); } catch (...) {}
    }
    if (!data.contains("ignored_users")) data["ignored_users"] = json::object();
    data["ignored_users"][user_id] = json::object();
    auto putResp = authPut("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/account_data/m.ignored_user_list",
        data.dump());
    return putResp.ok();
}

bool Client::unignoreUser(const std::string& user_id) {
    json data;
    auto resp = authGet("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/account_data/m.ignored_user_list");
    if (resp.ok()) {
        try { data = json::parse(resp.body); } catch (...) { return false; }
    }
    if (!data.contains("ignored_users")) return true;
    data["ignored_users"].erase(user_id);
    auto putResp = authPut("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/account_data/m.ignored_user_list",
        data.dump());
    return putResp.ok();
}

json Client::getPowerLevels(const std::string& room_id) {
    auto events = getRoomState(room_id);
    for (auto& ev : events) {
        if (ev.type == "m.room.power_levels") return ev.content;
    }
    return json::object();
}

bool Client::setPowerLevel(const std::string& room_id, const std::string& user_id, int level) {
    auto pl = getPowerLevels(room_id);
    if (pl.empty()) pl = {{"users", json::object()}, {"users_default", 0},
        {"events", {{"m.room.name", 50}, {"m.room.power_levels", 100}}}};
    pl["users"][user_id] = level;
    sendStateEvent(room_id, "m.room.power_levels", "", pl);
    return true;
}

bool Client::setRoomTag(const std::string& room_id, const std::string& tag, float order) {
    json content = {{"order", order}};
    auto resp = authPut("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/rooms/" + room_id + "/tags/" + tag,
        content.dump());
    return resp.ok();
}

bool Client::deleteRoomTag(const std::string& room_id, const std::string& tag) {
    auto resp = authDelete("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/rooms/" + room_id + "/tags/" + tag);
    return resp.ok();
}

json Client::getRoomTags(const std::string& room_id) {
    auto resp = authGet("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/rooms/" + room_id + "/tags");
    return resp.ok() ? json::parse(resp.body) : json::object();
}

json Client::getPinnedEvents(const std::string& room_id) {
    auto events = getRoomState(room_id);
    for (auto& ev : events) {
        if (ev.type == "m.room.pinned_events") return ev.content;
    }
    return json::object();
}

bool Client::pinEvent(const std::string& room_id, const std::string& event_id) {
    auto pinned = getPinnedEvents(room_id);
    if (!pinned.contains("pinned")) pinned["pinned"] = json::array();
    pinned["pinned"].push_back(event_id);
    sendStateEvent(room_id, "m.room.pinned_events", "", pinned);
    return true;
}

bool Client::unpinEvent(const std::string& room_id, const std::string& event_id) {
    auto pinned = getPinnedEvents(room_id);
    if (!pinned.contains("pinned")) return false;
    json newPinned = json::array();
    for (auto& ev : pinned["pinned"]) {
        if (ev.get<std::string>() != event_id) newPinned.push_back(ev);
    }
    sendStateEvent(room_id, "m.room.pinned_events", "", {{"pinned", newPinned}});
    return true;
}

std::string Client::upgradeRoom(const std::string& room_id, const std::string& new_version) {
    json content = {{"new_version", new_version}};
    auto resp = authPost("/_matrix/client/r0/rooms/" + room_id + "/upgrade", content.dump());
    checkResponse(resp);
    return json::parse(resp.body)["replacement_room"].get<std::string>();
}

bool Client::mirrorMessage(const std::string& from_room, const std::string& event_id,
                            const std::string& to_room) {
    auto events = getRoomMessages(from_room, event_id);
    if (events.empty()) return false;
    auto& ev = events[0];
    std::string body = ev.content.value("body", "(no content)");
    std::string sender = ev.sender;
    sendTextMessage(to_room, "[← " + from_room + "] <" + sender + "> " + body);
    return true;
}

bool Client::mirrorEvent(const std::string& from_room, const std::string& event_id,
                          const std::string& to_room) {
    return mirrorMessage(from_room, event_id, to_room);
}

json Client::getRoomStats(const std::string& room_id) {
    json stats;
    auto msgs = getRoomMessages(room_id, "", "b", 500);
    std::map<std::string, int> posters;
    int total = 0;
    for (auto& ev : msgs) { posters[ev.sender]++; total++; }
    stats["total_messages"] = total;
    stats["unique_posters"] = posters.size();
    json top = json::array();
    std::vector<std::pair<int, std::string>> sorted;
    for (auto& [k, v] : posters) sorted.push_back({v, k});
    std::sort(sorted.rbegin(), sorted.rend());
    for (size_t i = 0; i < std::min((size_t)10, sorted.size()); i++) {
        std::string sender = sorted[i].second;
        auto at = sender.find(':');
        if (at != std::string::npos && sender.starts_with("@")) sender = sender.substr(1, at - 1);
        top.push_back({{"sender", sender}, {"count", sorted[i].first}});
    }
    stats["top_posters"] = top;
    return stats;
}

std::string Client::exportRoom(const std::string& room_id, const std::string& format,
                                int limit) {
    auto msgs = getRoomMessages(room_id, "", "b", limit);
    std::ostringstream oss;

    if (format == "json") {
        json j;
        j["room_id"] = room_id;
        j["messages"] = json::array();
        for (auto& ev : msgs) {
            json m;
            m["sender"] = ev.sender; m["body"] = ev.content.value("body", ""); m["ts"] = ev.origin_server_ts;
            j["messages"].push_back(m);
        }
        return j.dump(2);
    } else if (format == "html") {
        oss << "<!DOCTYPE html><html><head><meta charset='utf-8'><title>" << room_id
            << "</title><style>body{font-family:monospace;max-width:800px;margin:auto;}"
            << ".msg{margin:8px 0;padding:8px;border-bottom:1px solid #ccc;}"
            << ".sender{font-weight:bold;color:#36c;}.time{color:#999;font-size:0.8em;}"
            << "</style></head><body><h1>" << room_id << "</h1>";
        for (auto& ev : msgs)
            oss << "<div class='msg'><span class='sender'>" << ev.sender << "</span>"
                << " <span class='time'>" << ev.origin_server_ts << "</span>"
                << "<div>" << ev.content.value("body", "") << "</div></div>";
        oss << "</body></html>";
        return oss.str();
    } else {
        oss << "=== " << room_id << " ===\n\n";
        for (auto& ev : msgs)
            oss << "[" << ev.sender << "] " << ev.content.value("body", "") << "\n";
        return oss.str();
    }
}

bool Client::setCustomStatus(const std::string& status, const std::string& emoji) {
    json content = {{"status", status}};
    if (!emoji.empty()) content["emoji"] = emoji;
    auto resp = authPut("/_matrix/client/r0/user/" +
        http::urlEncode(impl->creds.user_id) + "/account_data/im.vector.user_status", content.dump());
    return resp.ok();
}

}} // namespace matrixcli::matrix

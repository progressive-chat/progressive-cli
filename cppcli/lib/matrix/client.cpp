#include "client.hpp"
#include "error.hpp"
#include "../database/db.hpp"
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

    std::string txn_id = generateTxnId();
    std::string path = "/_matrix/client/r0/rooms/" + room_id +
                       "/send/m.room.message/" + txn_id;

    auto resp = authPut(path, content.dump());
    checkResponse(resp);

    auto j = json::parse(resp.body);
    return j["event_id"].get<std::string>();
}

std::string Client::sendTextMessage(const std::string& room_id,
                                     const std::string& body) {
    return sendMessage(room_id, body, "m.text");
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

                for (auto& ev : sr.account_data) onEvent(ev);
                for (auto& ev : sr.presence) onEvent(ev);
                for (auto& ev : sr.to_device) onEvent(ev);

                for (auto& [room_id, room] : sr.rooms.join) {
                    if (impl->db) impl->db->upsertRoom(room_id, room);
                    for (auto& ev : room.state.events) {
                        if (impl->db) impl->db->insertEvent(ev);
                        onEvent(ev);
                    }
                    for (auto& ev : room.timeline.events) {
                        if (impl->db) impl->db->insertEvent(ev);
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

}} // namespace matrixcli::matrix

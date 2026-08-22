#include "client.hpp"
#include "error.hpp"
#include "pushrules.hpp"
#include "client_impl.hpp"
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

std::string Client::uploadMediaChunked(const std::vector<uint8_t>& data,
                                       const std::string& filename,
                                       const std::string& content_type,
                                       int chunkCount) {
    const int64_t total = static_cast<int64_t>(data.size());
    if (total <= 0) throw std::runtime_error("uploadMedia: empty data");
    std::string ct = content_type.empty() ? "application/octet-stream" : content_type;

    struct ChunkResult { std::string uploadId; std::string mxc; };
    auto postChunk = [&](int64_t start, int64_t endExcl,
                         const std::string& uploadId) -> ChunkResult {
        std::ostringstream url;
        url << impl->homeserver_url << "/_matrix/media/v3/upload";
        if (!filename.empty() && uploadId.empty())
            url << "?filename=" << http::urlEncode(filename);
        if (!uploadId.empty()) {
            if (filename.empty()) url << "?";
            else url << "&";
            url << "uploadId=" << http::urlEncode(uploadId);
        }
        std::map<std::string, std::string> headers;
        headers["Content-Type"] = ct;
        if (!impl->creds.access_token.empty())
            headers["Authorization"] = "Bearer " + impl->creds.access_token;
        headers["Content-Range"] = "bytes " + std::to_string(start) + "-" +
                                    std::to_string(endExcl - 1) + "/" +
                                    std::to_string(total);
        std::string body(reinterpret_cast<const char*>(data.data() + start),
                         static_cast<size_t>(endExcl - start));
        auto resp = impl->http.post(url.str(), body, headers);
        if (!resp.ok())
            throw std::runtime_error("upload chunk failed: HTTP " +
                                     std::to_string(resp.status_code));
        auto j = json::parse(resp.body);
        ChunkResult r;
        if (j.contains("upload_id")) r.uploadId = j["upload_id"].get<std::string>();
        else if (j.contains("uploadId")) r.uploadId = j["uploadId"].get<std::string>();
        if (j.contains("content_uri")) r.mxc = j["content_uri"].get<std::string>();
        return r;
    };

    if (chunkCount <= 1) {
        ChunkResult r = postChunk(0, total, "");
        if (r.mxc.empty())
            throw std::runtime_error("upload: no content_uri in response");
        return r.mxc;
    }
    int64_t per = (total + static_cast<int64_t>(chunkCount) - 1) /
                  static_cast<int64_t>(chunkCount);  // ceil
    std::string uploadId;
    for (int i = 0; i < chunkCount; ++i) {
        int64_t start = i * per;
        int64_t endExcl = std::min(start + per, total);
        if (start >= endExcl) break;
        ChunkResult r = postChunk(start, endExcl, uploadId);
        if (!r.uploadId.empty()) uploadId = r.uploadId;
        if (!r.mxc.empty()) return r.mxc;
    }
    if (!uploadId.empty())
        throw std::runtime_error(
            "upload: server returned no content_uri after all chunks");
    throw std::runtime_error("upload: no content_uri in response");
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

json Client::searchUserDirectory(const std::string& search_term, int limit) {
    json body = {{"search_term", search_term}, {"limit", limit}};
    auto resp = authPost("/_matrix/client/r0/user_directory/search", body.dump());
    checkResponse(resp);
    auto j = json::parse(resp.body);
    // Add relevance scoring for better ordering
    if (j.contains("results")) {
        std::vector<json> sorted;
        for (auto& r : j["results"]) {
            int score = 0;
            std::string dn = r.value("display_name", "");
            std::string uid = r.value("user_id", "");
            if (dn == search_term) score += 100;
            else if (uid == search_term) score += 90;
            else if (dn.find(search_term) == 0) score += 60;
            else if (dn.find(search_term) != std::string::npos) score += 20;
            else if (uid.find(search_term) != std::string::npos) score += 10;
            r["_score"] = score;
            sorted.push_back(r);
        }
        std::sort(sorted.begin(), sorted.end(), [](const json& a, const json& b) {
            return a["_score"].get<int>() > b["_score"].get<int>();
        });
        j["results"] = sorted;
    }
    return j;
}

json Client::adminDeactivateUser(const std::string& user_id) {
    json body = {{"erase", false}};
    auto resp = authPost("/_synapse/admin/v1/deactivate/" + user_id, body.dump());
    return resp.ok() ? json::parse(resp.body) : json::object();
}

json Client::adminResetPassword(const std::string& user_id, const std::string& new_password) {
    json body = {{"new_password", new_password}};
    auto resp = authPost("/_synapse/admin/v1/reset_password/" + user_id, body.dump());
    return resp.ok() ? json::parse(resp.body) : json::object();
}

json Client::adminListUsers(int limit, const std::string& from) {
    std::string path = "/_synapse/admin/v2/users?limit=" + std::to_string(limit);
    if (!from.empty()) path += "&from=" + http::urlEncode(from);
    auto resp = authGet(path);
    return resp.ok() ? json::parse(resp.body) : json::object();
}

json Client::adminDeleteRoom(const std::string& room_id) {
    auto resp = authPost("/_synapse/admin/v1/rooms/" + room_id + "/delete", "{}");
    return resp.ok() ? json::parse(resp.body) : json::object();
}

json Client::adminShadowBan(const std::string& user_id) {
    auto resp = authPost("/_synapse/admin/v1/users/" + user_id + "/shadow_ban", "{}");
    return resp.ok() ? json::parse(resp.body) : json::object();
}

json Client::adminRoomStats() {
    auto resp = authGet("/_synapse/admin/v1/statistics/database/rooms");
    return resp.ok() ? json::parse(resp.body) : json::object();
}

}} // namespace matrixcli::matrix

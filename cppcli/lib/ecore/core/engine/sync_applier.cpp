// src/core/engine/sync_applier.cpp — moved from room_store (X1 phase 3).
// Worker-thread pure logic: sync response -> RoomSyncUpdate delta.
#include "sync_applier.hpp"

#include "../crypto/decryptor.hpp"
#include "core/debug_log.hpp"
#include "core/json_utils.hpp"

#include <simdjson.h>

namespace progressive::desktop {


// Parse a decrypted Megolm plaintext JSON into type + content JSON.
static bool parsePlaintextBody(const std::string& plaintextJson,
                               std::string& outType,
                               std::string& outContentJson) {
    simdjson::dom::parser parser;
    auto root = parser.parse(plaintextJson);
    if (root.error() != simdjson::SUCCESS) return false;
    auto t = root.value()["type"].get_string();
    if (t.error() != simdjson::SUCCESS) return false;
    outType = std::string(t.value());
    auto cr = root.value()["content"];
    if (cr.error() == simdjson::SUCCESS) {
        outContentJson = simdjson::to_string(cr.value());
    }
    return true;
}

std::string SyncApplier::extractStringDecAt(std::string_view json, const std::string& key,
                                              size_t startPos) {
    std::string p = "\"" + key + "\":\"";
    auto pos = json.find(p, startPos);
    if (pos == std::string_view::npos) { p = "\"" + key + "\": \""; pos = json.find(p, startPos); }
    if (pos != std::string_view::npos) { pos += p.size(); size_t end = pos;
        while (end < json.size()) { if (json[end]=='\\'&&end+1<json.size()){end+=2;continue;} if (json[end]=='"')break; ++end; }
        if (end < json.size()) return jsonUnescape(std::string(json.substr(pos, end-pos))); }
    return {};
}

std::string SyncApplier::extractStringDec(std::string_view json, const std::string& key) {
    return SyncApplier::extractStringDecAt(json, key, 0);
}

std::string SyncApplier::extractString(std::string_view json, const std::string& key) {
    return SyncApplier::extractStringDec(json, key);
}

// Extract the m.encrypted media object (file: / info.thumbnail_file):
// {url,key,iv,hashes:{sha256},v,mimetype}. Fills url/key/iv/sha.
static void extractEncryptedFile(const std::string& contentJson,
    const std::string& path, std::string& url, std::string& key,
    std::string& iv, std::string& sha, std::string* mimetype = nullptr) {
    simdjson::dom::parser p;
    auto doc = p.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) return;
    // path is "file" or a dotted path like "info.thumbnail_file"
    auto dot = path.find('.');
    simdjson::simdjson_result<simdjson::dom::element> obj;
    if (dot != std::string::npos) {
        auto base = doc.value()[std::string_view(path).substr(0, dot)];
        if (base.error() != simdjson::SUCCESS) return;
        obj = base.value()[std::string_view(path).substr(dot + 1)];
    } else {
        obj = doc.value()[path];
    }
    if (obj.error() != simdjson::SUCCESS) return;
    auto u = obj.value()["url"].get_string();
    if (u.error() == simdjson::SUCCESS) url = std::string(u.value());
    auto k = obj.value()["key"].get_string();
    if (k.error() == simdjson::SUCCESS) key = std::string(k.value());
    auto v = obj.value()["iv"].get_string();
    if (v.error() == simdjson::SUCCESS) iv = std::string(v.value());
    auto h = obj.value()["hashes"]["sha256"].get_string();
    if (h.error() == simdjson::SUCCESS) sha = std::string(h.value());
    if (mimetype) {
        auto mt = obj.value()["mimetype"].get_string();
        if (mt.error() == simdjson::SUCCESS) *mimetype = std::string(mt.value());
    }
}

std::string SyncApplier::extractThreadRootId(std::string_view json) {
    simdjson::dom::parser p;
    auto doc = p.parse(json);
    if (doc.error() != simdjson::SUCCESS) return {};
    auto rel = doc.value()["m.relates_to"];
    if (rel.error() != simdjson::SUCCESS) return {};
    auto rt = rel["rel_type"].get_string();
    if (rt.error() != simdjson::SUCCESS || std::string_view(rt.value()) != "m.thread") return {};
    auto eid = rel["event_id"].get_string();
    if (eid.error() != simdjson::SUCCESS) return {};
    return std::string(eid.value());
}

std::string SyncApplier::extractReplyToId(std::string_view contentJson) {
    simdjson::dom::parser p;
    auto doc = p.parse(contentJson);
    if (doc.error() != simdjson::SUCCESS) return {};
    auto rel = doc.value()["m.relates_to"];
    if (rel.error() != simdjson::SUCCESS) return {};
    auto rt = rel["rel_type"].get_string();
    if (rt.error() == simdjson::SUCCESS && std::string_view(rt.value()) == "m.in_reply_to") {
        auto eid = rel["event_id"].get_string();
        if (eid.error() != simdjson::SUCCESS) return {};
        return std::string(eid.value());
    }
    return {};
}

std::string SyncApplier::msgType(std::string_view json) { return SyncApplier::extractStringDec(json, "msgtype"); }
std::string SyncApplier::msgBody(std::string_view json) { return SyncApplier::extractStringDec(json, "body"); }

// ---- RoomStore ----

RoomMeta SyncApplier::extractRoomMeta(const FastRoom& room, const std::string& myUserId) {
    RoomMeta m;
    for (const auto& e : room.stateEvents) {
        if (e.type == "m.room.name" && m.name.empty() && !e.contentJson.empty())
            m.name = SyncApplier::extractStringDec(e.contentJson, "name");
        else if (e.type == "m.room.avatar" && m.avatarUrl.empty() && !e.contentJson.empty())
            m.avatarUrl = SyncApplier::extractStringDec(e.contentJson, "url");
        else if (e.type == "m.room.canonical_alias" && m.canonicalAlias.empty() && !e.contentJson.empty())
            m.canonicalAlias = SyncApplier::extractStringDec(e.contentJson, "alias");
        else if (e.type == "m.room.encryption") m.isEncrypted = true;
        else if (e.type == "m.room.member" && !e.contentJson.empty()) {
            auto mem = SyncApplier::extractString(e.contentJson, "membership");
            if (mem == "join" && std::string(e.stateKey) != myUserId) {
                if (m.dmDisplayName.empty()) m.dmDisplayName = SyncApplier::extractString(e.contentJson, "displayname");
                if (m.dmAvatarUrl.empty()) m.dmAvatarUrl = SyncApplier::extractStringDec(e.contentJson, "avatar_url");
            }
        }
    }
    for (const auto& e : room.timeline.events) {
        if (m.name.empty() && e.type == "m.room.name" && !e.contentJson.empty())
            m.name = SyncApplier::extractStringDec(e.contentJson, "name");
        if (m.avatarUrl.empty() && e.type == "m.room.avatar" && !e.contentJson.empty())
            m.avatarUrl = SyncApplier::extractStringDec(e.contentJson, "url");
        if (m.canonicalAlias.empty() && e.type == "m.room.canonical_alias" && !e.contentJson.empty())
            m.canonicalAlias = SyncApplier::extractStringDec(e.contentJson, "alias");
    }
    return m;
}

std::string SyncApplier::extractLastMessageBody(const std::vector<FastEvent>& events) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        // The newest encrypted event has no readable body — show a placeholder
        // instead of a stale/empty preview.
        if (it->type == "m.room.encrypted") return "[encrypted]";
        if (it->type == "m.room.message" && !it->contentJson.empty()) {
            simdjson::dom::parser p;
            auto doc = p.parse(it->contentJson);
            if (doc.error() != simdjson::SUCCESS) continue;
            auto b = doc.value()["body"].get_string();
            if (b.error() == simdjson::SUCCESS) return std::string(b.value());
        }
    }
    return {};
}

RoomSyncUpdate SyncApplier::prepareRoomSyncUpdate(const FastSyncResponse& resp,
                                                  const std::string& currentRoomId,
                                                  const std::string& myUserId) {
    RoomSyncUpdate u;

    // Left rooms
    for (const auto& leftId : resp.leftRoomIds)
        u.roomsToRemove.push_back(std::string(leftId));

    // Joined rooms
    for (const auto& [roomIdView, room] : resp.joinedRooms) {
        std::string roomId(roomIdView);
        RoomMeta meta = SyncApplier::extractRoomMeta(room, myUserId);

        RoomData rd;
        rd.roomId = roomId;
        // Name: meta.name → canonicalAlias → dmDisplayName → roomId
        rd.name = meta.name.empty()
            ? (meta.canonicalAlias.empty()
                ? (meta.dmDisplayName.empty() ? roomId : meta.dmDisplayName)
                : meta.canonicalAlias)
            : meta.name;
        rd.avatarUrl = meta.avatarUrl.empty() ? meta.dmAvatarUrl : meta.avatarUrl;
        rd.isEncrypted = meta.isEncrypted || room.isEncrypted;
        rd.lastMessage = jsonUnescape(SyncApplier::extractLastMessageBody(room.timeline.events));
        rd.lastActivityTs = room.timeline.events.empty() ? 0 : room.timeline.events.back().originServerTs;
        rd.unreadCount = room.notificationCount;
        rd.highlightCount = room.highlightCount;
        if (roomId == currentRoomId) {
            rd.unreadCount = 0;
            rd.highlightCount = 0;
        }
        for (auto& tu : room.typingUsers) rd.typingUsers.push_back(std::string(tu));

        // Store last notification body for highlights
        if (room.highlightCount > 0 && !room.timeline.events.empty()) {
            u.lastNotificationBody = SyncApplier::extractLastMessageBody(room.timeline.events);
        }

        u.roomsToUpsert.push_back(std::move(rd));

        // Member avatars for the current room: built from state + timeline
        // member events even on state-only updates (the avatar map must stay
        // current without new timeline events).
        if (roomId == currentRoomId) {
            for (const auto& e : room.stateEvents) {
                if (e.type == "m.room.member" && !e.contentJson.empty()) {
                    auto av = SyncApplier::extractStringDec(e.contentJson, "avatar_url");
                    if (!av.empty()) u.currentRoomAvatars[std::string(e.stateKey)] = av;
                    auto dn = SyncApplier::extractStringDec(e.contentJson, "displayname");
                    if (!dn.empty()) u.currentRoomMemberNames[std::string(e.stateKey)] = dn;
                }
            }
            for (const auto& e : room.timeline.events) {
                if (e.type == "m.room.member" && !e.contentJson.empty()) {
                    auto av = SyncApplier::extractStringDec(e.contentJson, "avatar_url");
                    if (!av.empty()) u.currentRoomAvatars[std::string(e.stateKey)] = av;
                    auto dn = SyncApplier::extractStringDec(e.contentJson, "displayname");
                    if (!dn.empty()) u.currentRoomMemberNames[std::string(e.stateKey)] = dn;
                }
            }
        }
        // Store timeline events for current room
        if (roomId == currentRoomId && !room.timeline.events.empty()) {
            u.currentRoomUpdated = true;
            u.currentRoomId = roomId;
            u.currentRoomEvents = room.timeline.events;
        }
    }

    // Invites
    for (const auto& inv : resp.invitedRooms) {
        std::string roomId(inv.roomId);
        RoomData rd;
        rd.roomId = roomId;
        rd.isInvite = true;
        rd.name = inv.roomName.empty() ? roomId : std::string(inv.roomName);
        rd.inviterId = std::string(inv.inviterId);
        if (!inv.inviterId.empty()) {
            std::string n = rd.inviterId;
            if (n[0] == '@') { auto c = n.find(':'); if (c != std::string::npos) n = n.substr(1, c-1); else n = n.substr(1); }
            rd.lastMessage = n + " invited you";
        }
        if (!inv.roomAvatar.empty()) rd.avatarUrl = std::string(inv.roomAvatar);
        rd.isEncrypted = inv.isEncrypted;
        rd.memberCount = inv.memberCount;
        if (rd.memberCount > 0) {
            rd.lastMessage += " · " + std::to_string(rd.memberCount) + " member" + (rd.memberCount > 1 ? "s" : "");
        }
        u.invitedRooms.push_back(std::move(rd));
    }

    u.inviteCount = static_cast<int>(u.invitedRooms.size());

    return u;
}

void SyncApplier::fastEventToDisplayed(const FastEvent& e, DisplayedEvent& de,
                                       const std::string& currentRoomId,
                                       Decryptor* decryptor) {
    de.eventId = std::string(e.eventId);
    de.senderId = std::string(e.senderId);
    de.type = std::string(e.type);
    de.contentJson = std::string(e.contentJson);
    de.originServerTs = e.originServerTs;
    if (!de.senderId.empty() && de.senderId[0] == '@') {
        auto colon = de.senderId.find(':');
        de.senderName = (colon != std::string::npos) ? de.senderId.substr(1, colon-1) : de.senderId.substr(1);
    }
    if (de.type == "m.room.encrypted" && decryptor && decryptor->isInitialized()) {
        LOG(LogChannel::E2EE, "fastEventToDisplayed: decrypting eid=%s", de.eventId.c_str());
        auto result = decryptor->decryptMegolmEvent(currentRoomId, de.senderId, de.contentJson, de.eventId, de.originServerTs);
        if (result.ok) {
            LOG(LogChannel::E2EE, "fastEventToDisplayed: DECRYPTED eid=%s", de.eventId.c_str());
            parsePlaintextBody(result.plaintext, de.type, de.contentJson);
        } else {
            LOG(LogChannel::E2EE, "fastEventToDisplayed: FAILED eid=%s err=%s",
                de.eventId.c_str(), result.error.c_str());
            de.decryptError = result.error;
            de.body = "[encrypted]"; de.msgtype = "m.notice";
        }
    } else if (de.type == "m.room.encrypted") {
        LOG(LogChannel::E2EE, "fastEventToDisplayed: SKIP decryptor=%p init=%d",
            (void*)decryptor, decryptor ? decryptor->isInitialized() : 0);
    }
    if (de.type == "m.room.message") {
        // String-based extraction (no simdjson DOM + no std::regex in the hot
        // per-event path — a Release-mode-only crash was observed in the DOM
        // region on CI; the extractors are equivalent and simpler).
        // Detect the formatted_body shape BEFORE extracting "body": in the
        // nested-object case {"formatted_body":{"body":"..."}} the naive
        // top-level extraction would grab the nested value.
        size_t fbKeyPos = de.contentJson.find("\"formatted_body\":");
        size_t fbValPos = fbKeyPos == std::string::npos ? std::string::npos
                                                        : fbKeyPos + 17;
        while (fbValPos != std::string::npos && fbValPos < de.contentJson.size() &&
               de.contentJson[fbValPos] == ' ')
            ++fbValPos;
        bool fbIsObject = fbValPos != std::string::npos &&
                          fbValPos < de.contentJson.size() &&
                          de.contentJson[fbValPos] == '{';
        std::string htmlBody;
        if (fbIsObject) {
            htmlBody = extractStringDecAt(de.contentJson, "body", fbValPos);
        } else {
            de.body = extractStringDec(de.contentJson, "body");  // extractor unescapes
            if (de.body.empty()) {
                // Plain formatted_body HTML string.
                htmlBody = extractStringDec(de.contentJson, "formatted_body");
            }
        }
        if (!htmlBody.empty()) {
            // Strip HTML tags without std::regex.
            std::string out;
            bool inTag = false;
            for (char c : htmlBody) {
                if (c == '<') inTag = true;
                else if (c == '>') inTag = false;
                else if (!inTag) out += c;
            }
            de.body = std::move(out);
        }
        de.msgtype = extractStringDec(de.contentJson, "msgtype");
        if (de.msgtype == "m.image" || de.msgtype == "m.video" ||
            de.msgtype == "m.file" || de.msgtype == "m.audio") {
            // Encrypted media (Element sends file: instead of url: in E2EE
            // rooms): url + base64 key/iv + sha256-of-plaintext + thumbnail.
            extractEncryptedFile(de.contentJson, "file", de.mxcUrl,
                                 de.mediaKey, de.mediaIv, de.mediaSha256, &de.mimetype);
            if (de.mxcUrl.empty())
                de.mxcUrl = extractStringDec(de.contentJson, "url");
            extractEncryptedFile(de.contentJson, "info.thumbnail_file",
                                 de.thumbUrl, de.thumbKey, de.thumbIv, de.thumbSha256);
            if (de.mimetype.empty())
                de.mimetype = extractStringDec(de.contentJson, "mimetype");
            if (de.body.empty())
                de.body = extractStringDec(de.contentJson, "filename");
        }
        auto thRoot = SyncApplier::extractThreadRootId(de.contentJson);
        if (!thRoot.empty()) { de.isThreadReply = true; de.threadRootId = thRoot; }
        // Bug fix: replies arriving via /sync never got isReply/replyToEventId.
        std::string replyTo = SyncApplier::extractReplyToId(de.contentJson);
        if (!replyTo.empty()) { de.isReply = true; de.replyToEventId = replyTo; }
        // Edits (m.relates_to: m.replace): carry the new_content body and the
        // target; the append path replaces the original row instead of
        // rendering a duplicate message.
        {
            simdjson::dom::parser pp;
            auto pd = pp.parse(de.contentJson);
            if (pd.error() == simdjson::SUCCESS) {
                auto rt = pd.value()["m.relates_to"]["rel_type"].get_string();
                auto reid = pd.value()["m.relates_to"]["event_id"].get_string();
                if (rt.error() == simdjson::SUCCESS &&
                    std::string_view(rt.value()) == "m.replace" &&
                    reid.error() == simdjson::SUCCESS) {
                    de.isReplace = true;
                    de.replaceTargetId = std::string(reid.value());
                    auto nb = pd.value()["m.new_content"]["body"].get_string();
                    if (nb.error() == simdjson::SUCCESS) de.body = std::string(nb.value());
                }
            }
        }
        stripReplyFallback(de);
    }
    if (de.type == "m.room.encrypted") {
        LOG(LogChannel::DBG, "sync-encrypted: sender=%s content=[%.300s]",
            de.senderId.c_str(), de.contentJson.c_str());
    }
    // Catch-all: log every event that passes through sync path
    LOG(LogChannel::DBG, "sync-event: type=%s bodyEmpty=%d contentEmpty=%d sender=%.30s body=[%.100s]",
        de.type.c_str(), (int)de.body.empty(), (int)de.contentJson.empty(),
        de.senderId.c_str(), de.body.c_str());
}

void SyncApplier::stripReplyFallback(DisplayedEvent& de) {
    if (!de.isReply || de.body.size() <= 2 || de.body[0] != '>' || de.body[1] != ' ')
        return;
    // Strip Element's fallback quote: "> <@user:server> text\n\nreal".
    auto nl = de.body.find('\n');
    if (nl != std::string::npos) {
        de.body.erase(0, nl + 1);
        if (!de.body.empty() && de.body[0] == '\n') de.body.erase(0, 1);
    }
}

std::string SyncApplier::makeSystemBody(const std::string& type, const std::string& contentJson,
                                    const std::string& stateKey) {
    if (type == "m.room.member") {
        std::string displayName = stateKey;
        auto colon = displayName.find(':');
        if (colon != std::string::npos && colon > 0 && displayName[0] == '@')
            displayName = displayName.substr(1, colon - 1);
        auto ms = SyncApplier::extractString(contentJson, "membership");
        if (ms == "join")      return displayName + " joined";
        else if (ms == "leave") return displayName + " left";
        else if (ms == "invite") return displayName + " was invited";
        else if (ms == "ban")   return displayName + " was banned";
        else return "";
    }
    else if (type == "m.room.topic") {
        auto topic = SyncApplier::extractString(contentJson, "topic");
        return "Topic changed" + (topic.empty() ? "" : ": " + topic);
    }
    else if (type == "m.room.name") {
        auto name = SyncApplier::extractString(contentJson, "name");
        return "Room renamed to " + (name.empty() ? "(removed)" : name);
    }
    else if (type == "m.room.encryption") {
        return "Encryption enabled";
    }
    else if (type == "m.room.create") {
        return "Room created";
    }
    else if (type == "m.room.avatar") {
        return "Avatar changed";
    }
    return "";
}

bool SyncApplier::decryptEventToDisplayed(const FastEvent& fe, DisplayedEvent& de,
                                             const std::string& currentRoomId,
                                             Decryptor* decryptor) {
    fastEventToDisplayed(fe, de, currentRoomId, decryptor);
    return true;
}

} // namespace progressive::desktop

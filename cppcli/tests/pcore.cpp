#include "pcore.hpp"

#include <cstdio>
#include <string>

#include "../lib/database/db.hpp"
#include "../lib/matrix/events.hpp"
#include "core/utils.hpp"

namespace matrixcli { namespace pcore {

Core& core() {
    static Core c;
    return c;
}

bool init() {
    Core& c = core();
    if (c.store) return c.storeOk;  // already initialized

    c.store = std::make_shared<progressive::desktop::SessionStore>();
    c.storeOk = c.store->open("session.db");
    if (!c.storeOk) {
        std::fprintf(stderr, "[pcore] cannot open session.db\n");
    }
    c.client = std::make_shared<progressive::desktop::MatrixClient>();
    c.client->setSessionStore(c.store.get());
    c.sync = std::make_unique<progressive::desktop::SyncEngine>();
    c.sync->setClient(c.client);
    c.sync->setSessionStore(c.store);
    return c.storeOk;
}

bool loadSavedSession() {
    Core& c = core();
    if (!c.storeOk) return false;
    auto acct = c.store->loadAccount();
    if (!acct) return false;
    c.client->setAccount(*acct);
    return c.client->isLoggedIn();
}

std::string bootstrap() {
    Core& c = core();
    if (!c.client || !c.client->isLoggedIn()) return "not logged in";

    auto acct = c.client->account();
    if (acct.deviceId.empty() || acct.deviceId == "PROGRESSIVE_DESKTOP") {
        acct.deviceId = progressive::desktop::generateUUID();
        c.client->setAccount(acct);
    }
    c.client->persistSession();

    auto r = c.sync->initializeE2EE();
    if (r.keysPublished) c.sync->uploadDeviceKeys();
    if (!r.e2eeOk) return "E2EE init failed (continuing without E2EE)";
    return "";
}

void startSync(const std::function<void(const progressive::desktop::FastSyncResponse&)>& onResponse) {
    Core& c = core();
    if (onResponse) c.sync->onSync(onResponse);
    c.sync->start();
}

void stopSync() {
    if (core().sync) core().sync->stop();
}

bool requireSession() {
    if (!init() || !loadSavedSession()) {
        std::fprintf(stderr, "Not logged in. Run 'progressive-cli login' first.\n");
        return false;
    }
    return true;
}

namespace {
// Timeline events that failed to decrypt (the room key had not arrived yet).
// The engine re-decrypts them in-memory once the key lands; the legacy cache
// must follow, or the view keeps showing "(no body)" forever (the event is
// past the since-token and never re-delivered).
struct PendingTimelineDecrypt {
    std::string roomId;
    std::string eventId;
    std::string senderId;
    std::string contentJson;
    int64_t originServerTs = 0;
};
std::vector<PendingTimelineDecrypt>& pendingTimelineDecrypts() {
    static std::vector<PendingTimelineDecrypt> v;
    return v;
}
}  // namespace

void feedCache(const progressive::desktop::FastSyncResponse& resp) {
    // Room metadata + timeline events -> legacy offline store, keeping
    // view/rooms/search/API working against the cache.
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return;

    // Retry the previously-failed decrypts first: the engine imported room
    // keys since the last sync, so these should now succeed (re-insert
    // REPLACEs the stale raw row).
    if (!pendingTimelineDecrypts().empty()) {
        auto& pend = pendingTimelineDecrypts();
        for (auto it = pend.begin(); it != pend.end();) {
            auto dec = core().sync->decryptor()->decryptMegolmEvent(
                it->roomId, it->senderId, it->contentJson, it->eventId, it->originServerTs);
            if (dec.ok && !dec.plaintext.empty()) {
                matrix::Event mev;
                mev.event_id = it->eventId;
                mev.room_id = it->roomId;
                mev.sender = it->senderId;
                mev.type = "m.room.message";
                mev.origin_server_ts = it->originServerTs;
                try {
                    auto j = nlohmann::json::parse(dec.plaintext);
                    if (j.contains("content") && j["content"].is_object()) j = j["content"];
                    mev.content = j;
                } catch (...) {}
                dbi.insertEvent(mev);
                it = pend.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& [roomIdView, room] : resp.joinedRooms) {
        std::string room_id(roomIdView);
        std::string room_name = room_id;
        std::string room_topic;
        std::string room_avatar;
        int member_count = 0;
        for (auto& ev : room.stateEvents) {
            std::string type(ev.type);
            try {
                auto cj = nlohmann::json::parse(std::string(ev.contentJson));
                if (type == "m.room.name") room_name = cj.value("name", room_id);
                else if (type == "m.room.topic") room_topic = cj.value("topic", "");
                else if (type == "m.room.avatar") room_avatar = cj.value("url", "");
                else if (type == "m.room.member" && ev.senderId == ev.stateKey && cj.value("membership", "") == "join") member_count++;
            } catch (...) {}
        }
        nlohmann::json rj;
        rj["name"] = room_name;
        rj["topic"] = room_topic;
        rj["avatar"] = room_avatar;
        rj["member_count"] = member_count;
        rj["is_encrypted"] = room.isEncrypted;
        dbi.upsertRoom(rj, room_id);

        for (auto& ev : room.timeline.events) {
            matrix::Event mev;
            mev.event_id = std::string(ev.eventId);
            mev.room_id = room_id;
            mev.sender = std::string(ev.senderId);
            mev.type = std::string(ev.type);
            mev.origin_server_ts = ev.originServerTs;
            std::string contentJson(ev.contentJson);
            if (room.isEncrypted && ev.type == "m.room.encrypted") {
                // Decrypt with the core decryptor so view/search/API see the
                // PLAINTEXT (the legacy cache stored the raw ciphertext —
                // every encrypted message showed "(no body)"). On failure
                // the raw content is stored AND the event is queued for a
                // re-decrypt pass once the room key arrives.
                auto dec = core().sync->decryptor()->decryptMegolmEvent(
                    room_id, std::string(ev.senderId), contentJson,
                    std::string(ev.eventId), ev.originServerTs);
                if (dec.ok && !dec.plaintext.empty()) {
                    try {
                        auto j = nlohmann::json::parse(dec.plaintext);
                        if (j.contains("content") && j["content"].is_object()) j = j["content"];
                        contentJson = j.dump();
                    } catch (...) { contentJson = dec.plaintext; }
                    mev.type = "m.room.message";
                } else {
                    pendingTimelineDecrypts().push_back(
                        {room_id, std::string(ev.eventId), std::string(ev.senderId),
                         contentJson, ev.originServerTs});
                    if (pendingTimelineDecrypts().size() > 4000)
                        pendingTimelineDecrypts().clear();
                }
            }
            try { mev.content = nlohmann::json::parse(contentJson); } catch (...) {}
            dbi.insertEvent(mev);
        }
    }

    // Invited rooms: remember the invitation in the cache — the room
    // metadata plus an invite member event. The invite DATE is the moment
    // the client first saw the invitation (origin_server_ts of the cached
    // invite event), so it survives restarts even when the server does
    // not re-report the invite state.
    std::string selfUserId;
    if (core().client) selfUserId = core().client->account().userId;
    if (!selfUserId.empty()) {
        auto existing = dbi.openInvites(selfUserId);
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for (auto& inv : resp.invitedRooms) {
            std::string roomId(inv.roomId);
            nlohmann::json rj;
            rj["name"] = std::string(inv.roomName);
            rj["avatar"] = std::string(inv.roomAvatar);
            rj["member_count"] = inv.memberCount;
            rj["is_encrypted"] = inv.isEncrypted;
            dbi.upsertRoom(rj, roomId);

            bool known = false;
            for (auto& ex : existing) {
                if (ex.roomId == roomId) { known = true; break; }
            }
            if (!known) {
                matrix::Event mev;
                mev.event_id = "$invite_" + roomId;
                mev.room_id = roomId;
                mev.sender = std::string(inv.inviterId);
                mev.type = "m.room.member";
                mev.state_key = selfUserId;
                mev.origin_server_ts = nowMs;
                mev.content = {{"membership", "invite"},
                               {"reason", std::string(inv.reason)}};
                dbi.insertEvent(mev);
            }
        }
    }
}

}} // namespace matrixcli::pcore

#include "pcore.hpp"

#include <cstdio>
#include <string>

#include "../lib/database/db.hpp"
#include "../lib/matrix/events.hpp"
#include "../lib/ecore/core/utils.hpp"

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
        std::fprintf(stderr, "Not logged in. Run 'matrixcli login' first.\n");
        return false;
    }
    return true;
}

void feedCache(const progressive::desktop::FastSyncResponse& resp) {
    // Room metadata + timeline events -> legacy offline store, keeping
    // view/rooms/search/API working against the cache.
    db::Database dbi;
    if (!dbi.open("matrixcli.db")) return;
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
            try { mev.content = nlohmann::json::parse(std::string(ev.contentJson)); } catch (...) {}
            dbi.insertEvent(mev);
        }
    }
}

}} // namespace matrixcli::pcore

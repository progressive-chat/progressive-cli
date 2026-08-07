#include "pcore.hpp"

#include <cstdio>
#include <string>

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

}} // namespace matrixcli::pcore

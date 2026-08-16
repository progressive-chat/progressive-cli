#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/matrix_client.hpp"
#include "core/session_store.hpp"
#include "core/sync_engine.hpp"
#include "core/fast_sync.hpp"

namespace matrixcli { namespace pcore {

// Single active session state over the vendored desktop core (lib/ecore).
struct Core {
    std::shared_ptr<progressive::desktop::MatrixClient> client;
    std::shared_ptr<progressive::desktop::SessionStore> store;
    std::unique_ptr<progressive::desktop::SyncEngine> sync;
    bool storeOk = false;
};

// Global ecore session state.
Core& core();

// Open session.db, construct client + store + sync engine.
// Returns false if the session store cannot be opened.
bool init();

// Load the saved account from the session store into the client.
// Returns true if a session was restored (client->isLoggedIn()).
bool loadSavedSession();

// Post-login bootstrap: ensure device id, persist session, init E2EE
// (olm account + device keys upload). Returns "" on success or a
// human-readable warning (E2EE failures are non-fatal).
std::string bootstrap();

// Start the sync loop. Every /sync response is handed to onResponse
// (used to feed the offline cache).
void startSync(const std::function<void(const progressive::desktop::FastSyncResponse&)>& onResponse);

// Stop the sync loop (blocks until the in-flight request finishes).
void stopSync();

// Session guard for commands: init + load the saved session; prints an
// error and returns false when there is no active session.
bool requireSession();

// Feed the offline cache (matrixcli.db) from a /sync response: room
// metadata + timeline events. Used by serve (every response) and by the
// one-shot `sync` command.
void feedCache(const progressive::desktop::FastSyncResponse& resp);

}} // namespace matrixcli::pcore

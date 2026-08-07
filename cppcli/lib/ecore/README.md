# ecore — vendored E2EE core from progressive-desktop.

Source provenance (byte-identical copies):

- progressive-desktop commit 6117567, directory src/core/
  (crypto/*, matrix_client/session_store/sync_engine/fast_sync/http_client
  headers, engine/ headers) — https://github.com/MaurerAnton/progressive-desktop
- progressive-android-experiments commit 8a5c6af6, directory
  progressive/src/main/cpp (progressive/olm.hpp + model headers in
  native/progressive/, model .cpp in native/) —
  https://github.com/MaurerAnton/progressive-android-experiments

Layout mirrors the desktop's src/ root: include dir is lib/ecore/, so
"core/crypto/..." and "core/version.h" resolve like the desktop build.
native/ is on the include path so <progressive/...> resolves.

Re-import procedure (after desktop gets fixes):

    D=/path/to/progressive-desktop
    PN=$D/third_party/progressive-android-experiments/progressive/src/main/cpp
    cp $D/src/core/crypto/*.{hpp,cpp} lib/ecore/core/crypto/
    cp $D/src/core/{account_info,debug_log,fast_sync,http_client,matrix_client,\
                   session_store,sync_engine,thread_pool,utils}.hpp lib/ecore/core/
    cp $D/src/core/http_client.cpp lib/ecore/core/
    cp $D/src/core/engine/*.hpp lib/ecore/core/engine/
    cp $PN/include/progressive/{olm,well_known,matrix_error,login_flow,sync_models,\
                   event_models,message_content,string_utils,auth_models,\
                   crypto_models,room_encryption,crypto_algorithms,megolm_decryptor}.hpp \
        lib/ecore/native/progressive/
    cp $PN/src/{well_known,matrix_error,login_flow,sync_models,event_models,message_content}.cpp \
        lib/ecore/native/
    git diff --stat lib/ecore   # review what changed upstream
    # core/version.h is a static copy — bump manually if the desktop version changes.

Local adaptations (keep minimal, always diff-able):
- core/version.h — static (desktop generates it via CMake configure_file)

Dependencies: libsodium (system), libcurl (system), OpenSSL (system),
simdjson v3.13.0 (FetchContent, same pin as desktop), olm 3.2.16 (vendored
at repo root).

Integration status:
- [x] builds as libmatrixcli_ecore.a (static) — full core: crypto/*, matrix_client,
      sync_engine, session_store, fast_sync, http_client, engine/*, thread_pool,
      memory_stats, json_utils + progressive_native Tier A modules needed by them
      (olm wrapper, models, megolm decryptor, olm_session, crypto_algorithms,
      room_encryption, string_utils, canonical_json, ...)
- [x] ported progressive-desktop test suite: e2ee_account, e2ee_otk_count,
      e2ee_sas, e2ee_store, olm_inbound, megolm_inbound, media_crypto,
      sync_applier — all green in ctest (10/10 total)
- [x] live-Synapse integration: test_synapse_e2ee ported; runs in CI via
      .github/workflows/synapse-e2ee.yml (fresh matrixdotorg/synapse container,
      registration + login rate limits raised); verified locally against a
      user-local Synapse — ALL SYNAPSE E2EE TESTS PASSED, ctest 11/11 with a
      reachable server (graceful SKIP without one)
- [ ] adapters: MatrixClient/SessionStore backed by CLI http/db
      (surface used by crypto: MatrixClient::queryKeys/sendToDevice/account/
      uploadRoomKeys/getRoomKeys/createRoomKeysVersion; SessionStore::saveBackupInfo)
      — NOTE: superseded — the full core is vendored and wired (pcore facade);
      this item is historical
- [x] wire decryptor/verification into CLI commands (e2ee status/upload/fallback,
      backup create/upload/restore/delete, crosssign setup/reset, ssss
      upload/retrieve)

Build notes:
- native/*.cpp compiled with -include progressive_compat.h (transitive STL
  includes, same as the desktop's progressive_native build)
- <android/log.h> resolves via native/android/log.h shim (fprintf-based)
- vendored libolm (repo root) Makefile: version script removed so the
  _olm_crypto_* internals used by crypto_algorithms.cpp are exported —
  matches the FetchContent/CMake olm build the desktop uses

Documented deviations from the desktop originals (reimport-ecore.sh skips):
- core/crash_handler.hpp: the "[crash] handler installed" stderr line is
  removed — every matrixcli invocation would print it
- tests/test_synapse_e2ee.cpp: g_runSuffix for idempotent re-runs
  (documented in the test header)

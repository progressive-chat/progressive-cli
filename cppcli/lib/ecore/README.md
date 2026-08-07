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
- [x] builds as libmatrixcli_ecore.a (static)
- [ ] adapters: MatrixClient/SessionStore backed by CLI http/db
      (surface used by crypto: MatrixClient::queryKeys/sendToDevice/account/
      uploadRoomKeys/getRoomKeys/createRoomKeysVersion; SessionStore::saveBackupInfo)
- [ ] wire decryptor/verification into CLI commands (E2EE verify, key backup)

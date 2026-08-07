# Vendored E2EE core from progressive-desktop (src/core, commit 6117567)
# + progressive_native model headers/cpps (commit 8a5c6af6).
# Byte-identical copies; keep namespace progressive::desktop untouched so
# future re-imports are plain diffs. See README.md for the re-import procedure.

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
find_package(CURL REQUIRED COMPONENTS HTTPS)

include(FetchContent)
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.13.0
)
set(SIMDJSON_DEVELOPMENT_MODE OFF CACHE BOOL "" FORCE)
set(SIMDJSON_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SIMDJSON_DISABLE_DEPRECATED_API ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

add_library(matrixcli_ecore STATIC
    core/fast_sync.cpp
    core/http_client.cpp
    core/json_utils.cpp
    core/matrix_client.cpp
    core/memory_stats.cpp
    core/session_store.cpp
    core/sync_engine.cpp
    core/thread_pool.cpp
    core/engine/room_state.cpp
    core/engine/sync_applier.cpp
    core/engine/timeline_state.cpp
    core/crypto/backup_crypto.cpp
    core/crypto/cross_sign.cpp
    core/crypto/decryptor.cpp
    core/crypto/ed25519.cpp
    core/crypto/key_backup.cpp
    core/crypto/media_crypto.cpp
    core/crypto/megolm_store.cpp
    core/crypto/olm_account.cpp
    core/crypto/random.cpp
    core/crypto/recovery_key.cpp
    core/crypto/sas.cpp
    core/crypto/sas_emojis.cpp
    core/crypto/sig_verify.cpp
    core/crypto/ssss.cpp
    core/crypto/verification.cpp
    core/crypto/verify_controller.cpp
    native/canonical_json.cpp
    native/content_utils.cpp
    native/crypto_algorithms.cpp
    native/event_models.cpp
    native/json_parser.cpp
    native/login_flow.cpp
    native/markdown.cpp
    native/matrix_error.cpp
    native/megolm_decryptor.cpp
    native/megolm_v4.cpp
    native/message_content.cpp
    native/olm.cpp
    native/olm_account_manager.cpp
    native/olm_manager.cpp
    native/olm_session.cpp
    native/room_encryption.cpp
    native/string_utils.cpp
    native/sync_models.cpp
    native/well_known.cpp
)

target_include_directories(matrixcli_ecore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/native
)

target_link_libraries(matrixcli_ecore PUBLIC
    olm
    simdjson::simdjson
    PkgConfig::SODIUM
    PkgConfig::SQLITE3
    CURL::libcurl
    OpenSSL::Crypto
)

# Vendored native code relies on transitive STL includes (Android NDK style);
# force-include the same compat shim the desktop build uses (gcc is stricter).
target_compile_options(matrixcli_ecore PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-include>
    $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/native/progressive_compat.h>
)

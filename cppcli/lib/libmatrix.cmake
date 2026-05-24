# libmatrix — C++20 Matrix client library
# 
# Usage as submodule:
#   add_subdirectory(path/to/libmatrix)
#   target_link_libraries(your_app PRIVATE libmatrix)
#
# The library exposes these targets:
#   libmatrix          — umbrella (core + e2ee + db + http)
#   libmatrix-core     — Matrix CS API client only
#   libmatrix-http     — raw socket HTTP client
#   libmatrix-e2ee     — E2EE (libolm wrappers)
#   libmatrix-db       — SQLite storage

# Dependencies
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    FetchContent_MakeAvailable(json)
endif()

# libolm
if(NOT TARGET olm)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOLM REQUIRED olm)
    add_library(olm SHARED IMPORTED)
    find_library(LIBOLM_LIBRARY_PATH olm REQUIRED)
    set_target_properties(olm PROPERTIES
        IMPORTED_LOCATION "${LIBOLM_LIBRARY_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOLM_INCLUDE_DIRS}")
endif()

# Components
if(NOT TARGET libmatrix-http)
    add_library(libmatrix-http STATIC http/http.cpp http/proxy.cpp)
    target_include_directories(libmatrix-http PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(libmatrix-http PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endif()

if(NOT TARGET libmatrix-e2ee)
    add_library(libmatrix-e2ee STATIC e2ee/olm.cpp e2ee/megolm.cpp e2ee/crypto.cpp e2ee/session.cpp)
    target_include_directories(libmatrix-e2ee PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(libmatrix-e2ee PUBLIC olm Threads::Threads)
    target_compile_definitions(libmatrix-e2ee PUBLIC MATRIXCLI_HAS_E2EE=1)
endif()

if(NOT TARGET libmatrix-db)
    add_library(libmatrix-db STATIC database/db.cpp)
    target_include_directories(libmatrix-db PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${SQLITE3_INCLUDE_DIRS})
    target_link_libraries(libmatrix-db PUBLIC ${SQLITE3_LIBRARIES} nlohmann_json::nlohmann_json)
endif()

if(NOT TARGET libmatrix-core)
    add_library(libmatrix-core STATIC
        matrix/client.cpp matrix/events.cpp matrix/error.cpp
        matrix/auth.cpp matrix/sync.cpp matrix/api.cpp
        matrix/pushrules.cpp matrix/room_sort.cpp matrix/sliding_sync.cpp)
    target_include_directories(libmatrix-core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(libmatrix-core PUBLIC libmatrix-http libmatrix-db libmatrix-e2ee
        nlohmann_json::nlohmann_json Threads::Threads)
endif()

if(NOT TARGET libmatrix)
    add_library(libmatrix INTERFACE)
    target_link_libraries(libmatrix INTERFACE libmatrix-core libmatrix-e2ee libmatrix-db libmatrix-http)
endif()

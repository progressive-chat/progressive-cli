cmake_minimum_required(VERSION 3.20)
project(matrixcli VERSION 0.5.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build tests" ON)
option(BUILD_TUI "Build terminal UI" ON)
option(USE_SYSTEM_OLM "Use system libolm" OFF)

# --- Canonical core: progressive-chat/progressive-core (FetchContent) ---
# The shared Qt-free core (matrix_client, sync_engine, decryptor, …) — it
# also brings its own libolm 3.2.16, which is why the olm import below is
# guarded by if(NOT TARGET olm).
include(FetchContent)
FetchContent_Declare(
    progressive-core
    GIT_REPOSITORY https://github.com/progressive-chat/progressive-core.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
set(PROGRESSIVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Download the prebuilt core for this commit (fast builds). Falls back to
# compiling from source automatically when the artifact isn't published yet.
# Cross builds (e.g. armv7) pass -DPROGRESSIVE_CORE_USE_ARTIFACT=OFF to
# build the core from source — the prebuilt artifact is x86_64/aarch64.
if(NOT DEFINED PROGRESSIVE_CORE_USE_ARTIFACT)
    set(PROGRESSIVE_CORE_USE_ARTIFACT ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(progressive-core)
# Keep the historical target name so all consumers (api, src, tui) link
# unchanged against the fetched core.
add_library(matrixcli_ecore ALIAS progressive_core)

# libolm — only when the fetched canonical core hasn't provided one yet
# (progressive-core brings its own olm 3.2.16 via FetchContent).
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# libolm — the fetched canonical core (progressive-core) brings its own
# olm 3.2.16 via FetchContent (cached in the build tree). The system
# libolm is the fallback knob for the packagers (USE_SYSTEM_OLM=ON).
if(NOT TARGET olm)
    if(USE_SYSTEM_OLM)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBOLM REQUIRED olm)
        add_library(olm SHARED IMPORTED)
        find_library(LIBOLM_LIBRARY_PATH olm REQUIRED)
        set_target_properties(olm PROPERTIES
            IMPORTED_LOCATION "${LIBOLM_LIBRARY_PATH}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBOLM_INCLUDE_DIRS}"
        )
    else()
        message(FATAL_ERROR "The canonical core must provide the olm target")
    endif()
else()
    set(OLM_FOUND TRUE)
endif()

# nlohmann/json
include(FetchContent)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

add_subdirectory(lib)
add_subdirectory(src)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

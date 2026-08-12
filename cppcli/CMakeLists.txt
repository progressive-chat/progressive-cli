cmake_minimum_required(VERSION 3.20)
project(matrixcli VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
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

# libolm — only when the fetched canonical core hasn't provided one yet
# (progressive-core brings its own olm 3.2.16 via FetchContent).
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
    set(OLM_FOUND TRUE)
else()
    set(OLM_LIB_FILE "${CMAKE_SOURCE_DIR}/../libolm/build/libolm.so" CACHE FILEPATH "libolm shared library")
    if(NOT EXISTS "${OLM_LIB_FILE}")
        execute_process(
            COMMAND make -C "${CMAKE_SOURCE_DIR}/../libolm" -j
            RESULT_VARIABLE olm_build_result
        )
        if(NOT olm_build_result EQUAL 0)
            message(WARNING "Failed to build libolm. E2EE features will be disabled.")
            set(OLM_FOUND FALSE)
        else()
            set(OLM_FOUND TRUE)
        endif()
    else()
        set(OLM_FOUND TRUE)
    endif()
    if(OLM_FOUND)
        add_library(olm SHARED IMPORTED)
        set_target_properties(olm PROPERTIES
            IMPORTED_LOCATION "${OLM_LIB_FILE}"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/../libolm/include"
        )
    endif()
endif()  # USE_SYSTEM_OLM
else()
    # olm comes from FetchContent(progressive-core).
    set(OLM_FOUND TRUE)
endif()  # NOT TARGET olm

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

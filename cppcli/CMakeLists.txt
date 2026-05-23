cmake_minimum_required(VERSION 3.20)
project(matrixcli VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build tests" ON)
option(BUILD_TUI "Build terminal UI" ON)
option(USE_SYSTEM_OLM "Use system libolm" OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# libolm
if(USE_SYSTEM_OLM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOLM REQUIRED olm)
    add_library(olm SHARED IMPORTED)
    set_target_properties(olm PROPERTIES
        IMPORTED_LOCATION "${LIBOLM_LINK_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBOLM_INCLUDE_DIRS}"
    )
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

add_subdirectory(util)
add_subdirectory(json)
add_subdirectory(http)
add_subdirectory(matrix)
add_subdirectory(e2ee)
add_subdirectory(database)
add_subdirectory(api)
add_subdirectory(formats)
add_subdirectory(tdlib)
add_subdirectory(lemmy)
add_subdirectory(irc)
add_subdirectory(deltachat)
# matrixcli_ecore is provided by the top-level CMakeLists via
# FetchContent(progressive-core) — see cppcli/CMakeLists.txt.

if(BUILD_TUI)
    add_subdirectory(tui)
endif()

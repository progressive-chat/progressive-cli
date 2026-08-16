set(SOURCES
    main.cpp
    demo_tui.cpp
    config.cpp
    commands.cpp
    bridge_commands.cpp
    pcore.cpp
    e2ee_commands.cpp
    room_commands.cpp
    matrix_commands.cpp
    agent_commands.cpp
    agent_tools.cpp
    matrix_agent.cpp
    call_commands.cpp
    proxy_commands.cpp
    ascii_ui.cpp
    ascii_settings.cpp
    cli/args.cpp
    server/server.cpp
    server/web_proxy.cpp
)

add_executable(progressive-cli ${SOURCES})
target_include_directories(progressive-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(progressive-cli PRIVATE
    matrixcli_util
    matrixcli_json
    matrixcli_http
    matrixcli_matrix
    matrixcli_e2ee
    matrixcli_db
    matrixcli_api
    matrixcli_formats
    matrixcli_tdlib
    matrixcli_lemmy
    matrixcli_irc
    matrixcli_deltachat
    matrixcli_ecore
    Threads::Threads
)

if(BUILD_TUI)
    target_link_libraries(progressive-cli PRIVATE matrixcli_tui)
    target_compile_definitions(progressive-cli PRIVATE BUILD_TUI=1)
endif()

set(SOURCES
    main.cpp
    main_commands.hpp
    cmd_serve_login.cpp
    cmd_status.cpp
    cmd_view_attach.cpp
    cmd_bridges.cpp
    demo_tui.cpp
    demo_tui.hpp
    demo_data.cpp
    demo_data_extras.cpp
    demo_repl.cpp
    demo_showcases.cpp
    demo_showcases.hpp
    cmd_tui_commands.cpp
    cmd_tui_events.cpp
    config.cpp
    commands.cpp
    bridge_commands.cpp
    pcore.cpp
    e2ee_commands.cpp
    room_commands.cpp
    matrix_commands.cpp
    agent_commands.cpp
    agent_tools_file.cpp
    agent_tools_proc.cpp
    agent_tools_llm.cpp
    agent_tools_state.cpp
    agent_tools_run.cpp
    matrix_agent.cpp
    call_commands.cpp
    proxy_commands.cpp
    ascii_ui.cpp
    ascii_ui_text.cpp
    ascii_ui_rooms.cpp
    ascii_ui_impl.hpp
    ascii_ui_about.cpp
    ascii_ui_helpers.cpp
    ascii_ui_draw.cpp
    ascii_ui_draw_center.cpp
    ascii_ui_repl.cpp
    ascii_ui_repl_a.cpp
    ascii_ui_repl_b.cpp
    ascii_ui_repl_g.cpp
    ascii_ui_agent_repl.cpp
    ascii_ui_repl_e.cpp
    ascii_settings.cpp
    ascii_commands.cpp
    media_send.cpp
    cli/args.cpp
    server/server.cpp
    server/web_proxy.cpp
)

add_executable(progressive-cli-tests ${SOURCES})
target_include_directories(progressive-cli-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(progressive-cli-tests PRIVATE
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
    target_link_libraries(progressive-cli-tests PRIVATE matrixcli_tui)
    target_compile_definitions(progressive-cli-tests PRIVATE BUILD_TUI=1)
endif()

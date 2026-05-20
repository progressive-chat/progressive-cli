set(SOURCES
    main.cpp
    config.cpp
    cli/args.cpp
    server/server.cpp
)

add_executable(matrixcli ${SOURCES})
target_include_directories(matrixcli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli PRIVATE
    matrixcli_util
    matrixcli_json
    matrixcli_http
    matrixcli_matrix
    matrixcli_e2ee
    matrixcli_api
    matrixcli_formats
    Threads::Threads
)

if(BUILD_TUI)
    target_link_libraries(matrixcli PRIVATE matrixcli_tui)
    target_compile_definitions(matrixcli PRIVATE BUILD_TUI=1)
endif()

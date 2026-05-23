add_library(matrixcli_tui STATIC
    screen.cpp
    login.cpp
    main_view.cpp
    chat_view.cpp
    help_screen.cpp
    config.cpp
)
target_include_directories(matrixcli_tui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_tui PUBLIC matrixcli_util matrixcli_matrix matrixcli_db nlohmann_json::nlohmann_json)

if(TARGET Curses::Curses)
    target_link_libraries(matrixcli_tui PUBLIC Curses::Curses)
    target_compile_definitions(matrixcli_tui PUBLIC HAS_NCURSES)
else()
    find_package(Curses QUIET)
    if(CURSES_FOUND)
        target_link_libraries(matrixcli_tui PUBLIC ${CURSES_LIBRARIES})
        target_include_directories(matrixcli_tui PUBLIC ${CURSES_INCLUDE_DIR})
        target_compile_definitions(matrixcli_tui PUBLIC HAS_NCURSES)
    endif()
endif()

add_library(matrixcli_irc STATIC
    irc_client.cpp
)
target_include_directories(matrixcli_irc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_irc PUBLIC matrixcli_util Threads::Threads)

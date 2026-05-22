add_library(matrixcli_util STATIC
    logger.cpp
    string_utils.cpp
    notifications.cpp
)
target_include_directories(matrixcli_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_util PUBLIC matrixcli_json Threads::Threads)

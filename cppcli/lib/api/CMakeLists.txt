add_library(matrixcli_api STATIC
    server.cpp
    router.cpp
    handler.cpp
    format.cpp
)
target_include_directories(matrixcli_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_api PUBLIC matrixcli_util matrixcli_json Threads::Threads matrixcli_matrix)

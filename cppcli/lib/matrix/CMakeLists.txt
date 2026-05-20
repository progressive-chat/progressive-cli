add_library(matrixcli_matrix STATIC
    client.cpp
    events.cpp
    error.cpp
    auth.cpp
)
target_include_directories(matrixcli_matrix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_matrix PUBLIC
    matrixcli_http
    nlohmann_json::nlohmann_json
    Threads::Threads
)

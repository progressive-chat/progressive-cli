add_library(matrixcli_lemmy STATIC
    lemmy_client.cpp
)
target_include_directories(matrixcli_lemmy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_lemmy PUBLIC matrixcli_http nlohmann_json::nlohmann_json)

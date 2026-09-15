# matrixcli_gomuks — gomuks backend client: RFC 6455 websocket transport
# (ws_client) + jsoncmd session (gomuks_client). Links the HTTP client
# for the POST /_gomuks/auth login step.
add_library(matrixcli_gomuks STATIC
    ws_client.cpp
    gomuks_client.cpp
)
target_include_directories(matrixcli_gomuks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_gomuks PUBLIC
    matrixcli_http
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
)

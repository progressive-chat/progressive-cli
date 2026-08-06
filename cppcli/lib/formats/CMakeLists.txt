add_library(matrixcli_formats STATIC
    json.cpp
    text.cpp
    markdown.cpp
    gemini.cpp
    html.cpp
)
target_include_directories(matrixcli_formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_formats PUBLIC matrixcli_util matrixcli_json matrixcli_api)

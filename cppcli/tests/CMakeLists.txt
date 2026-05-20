add_executable(matrixcli_test
    test_main.cpp
)
target_include_directories(matrixcli_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(matrixcli_test PRIVATE
    matrixcli_util
    matrixcli_http
    matrixcli_matrix
    matrixcli_api
    matrixcli_formats
)

add_test(NAME matrixcli_test COMMAND matrixcli_test)

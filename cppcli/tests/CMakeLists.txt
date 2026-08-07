add_executable(matrixcli_test
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
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

# Vendored desktop E2EE core smoke test
add_executable(matrixcli_ecore_test
    test_ecore.cpp
)
target_include_directories(matrixcli_ecore_test PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(matrixcli_ecore_test PRIVATE
    matrixcli_ecore
)
add_test(NAME ecore_smoke COMMAND matrixcli_ecore_test)

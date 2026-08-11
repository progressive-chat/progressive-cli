add_library(matrixcli_tdlib STATIC
    tdlib_bridge.cpp
)
target_include_directories(matrixcli_tdlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_tdlib PUBLIC
    matrixcli_util
    ${CMAKE_DL_LIBS}
)

add_library(matrixcli_deltachat STATIC
    dc_bridge.cpp
)
target_include_directories(matrixcli_deltachat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_deltachat PUBLIC matrixcli_util ${CMAKE_DL_LIBS})

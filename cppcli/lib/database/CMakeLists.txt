find_package(PkgConfig)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

add_library(matrixcli_db STATIC
    db.cpp
)
target_include_directories(matrixcli_db PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SQLITE3_INCLUDE_DIRS}
)
target_link_libraries(matrixcli_db PUBLIC
    ${SQLITE3_LIBRARIES}
    matrixcli_matrix
    matrixcli_json
)

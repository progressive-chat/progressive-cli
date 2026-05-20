add_library(matrixcli_e2ee STATIC
    olm.cpp
    megolm.cpp
    crypto.cpp
    session.cpp
)

target_include_directories(matrixcli_e2ee PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(matrixcli_e2ee PUBLIC
    matrixcli_util
    nlohmann_json::nlohmann_json
)

if(OLM_FOUND)
    target_link_libraries(matrixcli_e2ee PUBLIC olm)
    target_compile_definitions(matrixcli_e2ee PUBLIC MATRIXCLI_HAS_E2EE=1)
else()
    target_compile_definitions(matrixcli_e2ee PUBLIC MATRIXCLI_HAS_E2EE=0)
endif()

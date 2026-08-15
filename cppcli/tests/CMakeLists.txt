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

# The LLM SSE assembly + the content extraction (the production edge cases).
add_executable(matrixcli_llm_test
    test_llm.cpp
)
target_include_directories(matrixcli_llm_test PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(matrixcli_llm_test PRIVATE
    matrixcli_util
    matrixcli_json
)
add_test(NAME llm_sse COMMAND matrixcli_llm_test)

# The db settings + the invite queries (the state_key fix).
add_executable(matrixcli_db_settings_test
    test_db_settings.cpp
)
target_include_directories(matrixcli_db_settings_test PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(matrixcli_db_settings_test PRIVATE
    matrixcli_db
    matrixcli_matrix
)
add_test(NAME db_settings COMMAND matrixcli_db_settings_test)

# The LLM session store (the paths, the sanitization, the archiving).
add_executable(matrixcli_sessions_test
    test_sessions.cpp
)
target_include_directories(matrixcli_sessions_test PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(matrixcli_sessions_test PRIVATE
    matrixcli_util
)
add_test(NAME sessions_store COMMAND matrixcli_sessions_test)

# Vendored desktop E2EE core smoke test
add_executable(matrixcli_ecore_test
    test_ecore.cpp
)
target_include_directories(matrixcli_ecore_test PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(matrixcli_ecore_test PRIVATE
    matrixcli_ecore
)
add_test(NAME ecore_smoke COMMAND matrixcli_ecore_test)

# Live-Synapse E2EE integration test (ported from progressive-desktop). Skips
# gracefully when no homeserver is reachable on SYNAPSE_URL (default :8008).
add_executable(matrixcli_synapse_e2ee_test test_synapse_e2ee.cpp)
target_link_libraries(matrixcli_synapse_e2ee_test PRIVATE matrixcli_ecore)
add_test(NAME synapse_e2ee COMMAND matrixcli_synapse_e2ee_test)

# CLI end-to-end test: drives the real binary against a live homeserver.
# Skips gracefully without one. Needs a shell + curl + grep.
find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM)
    add_test(NAME cli_e2e
             COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_e2e.sh
                     $<TARGET_FILE:matrixcli>)
endif()

# Ported progressive-desktop tests (vendored ecore E2EE/core suite)
foreach(t e2ee_account e2ee_otk_count e2ee_sas e2ee_store olm_inbound megolm_inbound media_crypto sync_applier e2ee_verify_protocol phase1 phase4)
    add_executable(matrixcli_${t}_test test_${t}.cpp)
    target_link_libraries(matrixcli_${t}_test PRIVATE matrixcli_ecore)
    add_test(NAME ${t} COMMAND matrixcli_${t}_test)
endforeach()

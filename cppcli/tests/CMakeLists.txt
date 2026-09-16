# Test executables: each test_*.cpp becomes its own CTest entry.
# They link against the matrixcli_main shared library so we don't
# recompile the whole source tree for each test.

# Core test library dependencies
set(TEST_LIBS
    matrixcli_main
    matrixcli_util
    matrixcli_json
    matrixcli_http
    matrixcli_matrix
    matrixcli_e2ee
    matrixcli_db
    matrixcli_api
    matrixcli_formats
    matrixcli_tdlib
    matrixcli_lemmy
    matrixcli_irc
    matrixcli_deltachat
    matrixcli_gomuks
    matrixcli_ecore
    Threads::Threads
)

# test_next: standalone regression tests for the "next" branch
add_executable(test_next test_next.cpp)
target_link_libraries(test_next PRIVATE matrixcli_http)
add_test(NAME next COMMAND test_next)

# test_gomuks_ws: M1 conformance for the gomuks backend client
add_executable(test_gomuks_ws test_gomuks_ws.cpp)
target_link_libraries(test_gomuks_ws PRIVATE ${TEST_LIBS})
add_test(NAME gomuks_ws COMMAND test_gomuks_ws)

# Explicit list of individual test files (avoids glob + duplicate issues)
set(INDIVIDUAL_TESTS
    test_agent_loop.cpp
    test_db_settings.cpp
    test_e2ee_account.cpp
    test_e2ee_otk_count.cpp
    test_e2ee_sas.cpp
    test_e2ee_store.cpp
    test_e2ee_verify_protocol.cpp
    test_ecore.cpp
    test_llm.cpp
    test_media_crypto.cpp
    test_media_send.cpp
    test_megolm_inbound.cpp
    test_olm_inbound.cpp
    test_phase1.cpp
    test_phase4.cpp
    test_sessions.cpp
    test_synapse_e2ee.cpp
    test_synapse_e2ee_crypto.cpp
    test_sync_applier.cpp
)

# Register each as its own test executable
foreach(test_src ${INDIVIDUAL_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE ${TEST_LIBS})
    if(BUILD_TUI)
        target_compile_definitions(${test_name} PRIVATE BUILD_TUI=1)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# test_main.cpp is the legacy catch-all; build it but don't add as CTest
# (it duplicates other tests). Kept for backward compatibility.
add_executable(test_main test_main.cpp)
target_link_libraries(test_main PRIVATE ${TEST_LIBS})
if(BUILD_TUI)
    target_compile_definitions(test_main PRIVATE BUILD_TUI=1)
endif()
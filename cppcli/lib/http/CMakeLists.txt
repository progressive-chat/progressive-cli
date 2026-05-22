add_library(matrixcli_http STATIC
    http.cpp
    proxy.cpp
)
target_include_directories(matrixcli_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matrixcli_http PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

set(CAMPELLO_NET_TEST_SOURCES
    test_version.cpp
    test_transport.cpp
    test_encrypted_transport.cpp
    test_connection_token.cpp
    test_serialization.cpp
    test_network_manager.cpp
    test_rate_limiter.cpp
    test_network_entity.cpp
    test_network_replication.cpp
    test_rpc.cpp
    test_prediction.cpp
    test_network_time.cpp
    test_memory_audit.cpp
    test_crypto.cpp
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    list(APPEND CAMPELLO_NET_TEST_SOURCES
        test_discovery.cpp
    )
else()
    list(APPEND CAMPELLO_NET_TEST_SOURCES
        test_wasm_transport.cpp
    )
endif()

add_executable(campello_net_tests ${CAMPELLO_NET_TEST_SOURCES})

target_link_libraries(campello_net_tests
    PRIVATE
        campello::net
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(campello_net_tests)

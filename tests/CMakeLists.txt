add_executable(campello_net_tests
    test_version.cpp
    test_transport.cpp
    test_serialization.cpp
    test_network_manager.cpp
    test_rate_limiter.cpp
    test_network_entity.cpp
    test_network_replication.cpp
    test_rpc.cpp
    test_prediction.cpp
    test_discovery.cpp
)

target_link_libraries(campello_net_tests
    PRIVATE
        campello::net
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(campello_net_tests)

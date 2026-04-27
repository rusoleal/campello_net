#include "campello_net/transport/emscripten_websocket_transport.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace systems::leal::campello_net::transport;

TEST_CASE("EmscriptenWebSocketTransport bind returns false", "[wasm][transport]") {
    EmscriptenWebSocketTransport transport;
    Address addr("127.0.0.1", 8080);
    REQUIRE(!transport.bind(addr));
}

TEST_CASE("EmscriptenWebSocketTransport initial state is disconnected", "[wasm][transport]") {
    EmscriptenWebSocketTransport transport;
    REQUIRE(!transport.is_connected());
    REQUIRE(transport.rtt() == 0.0f);
    REQUIRE(transport.packet_loss() == 0.0f);
}

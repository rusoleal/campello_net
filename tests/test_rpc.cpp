#include "campello_net/network_manager.hpp"
#include "campello_net/rpc_manager.hpp"
#include "campello_net/serialization/bit_stream.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <thread>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::transport;
using namespace systems::leal::campello_net::serialization;

static bool float_eq(float a, float b) {
    return a == b;
}

// ── Unit tests ──────────────────────────────────────────────────────────────

TEST_CASE("RpcManager serializes and deserializes arguments") {
    BitStream stream;
    serialize(stream, 42);
    serialize(stream, 3.14f);
    serialize(stream, std::string("hello"));

    int i = 0;
    float f = 0.0f;
    std::string s;
    REQUIRE(deserialize(stream, i));
    REQUIRE(deserialize(stream, f));
    REQUIRE(deserialize(stream, s));

    REQUIRE(i == 42);
    REQUIRE(float_eq(f, 3.14f));
    REQUIRE(s == "hello");
}

// ── Integration tests ───────────────────────────────────────────────────────

TEST_CASE("Client calls RPC on server") {
    Address server_addr("::1", 34582);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    // Connect
    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);
    REQUIRE(client_net.local_client_id() != 0);

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);

    RpcManager client_rpc;
    client_rpc.set_network_manager(&client_net);

    server_net.set_rpc_manager(&server_rpc);
    client_net.set_rpc_manager(&client_rpc);

    // Server registers a handler
    int received_value = 0;
    ClientId received_sender = 0;
    server_rpc.register_handler(7, [&received_value, &received_sender](ClientId sender, BitStream& args) {
        received_sender = sender;
        REQUIRE(deserialize(args, received_value));
    });

    // Client invokes RPC
    client_rpc.invoke_server(7, 123);

    // Poll to deliver
    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_value == 123);
    REQUIRE(received_sender == client_net.local_client_id());

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Server calls RPC on client") {
    Address server_addr("::1", 34583);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);
    ClientId client_id = client_net.local_client_id();
    REQUIRE(client_id != 0);

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);

    RpcManager client_rpc;
    client_rpc.set_network_manager(&client_net);

    server_net.set_rpc_manager(&server_rpc);
    client_net.set_rpc_manager(&client_rpc);

    float received_value = 0.0f;
    client_rpc.register_handler(3, [&received_value](ClientId /*sender*/, BitStream& args) {
        REQUIRE(deserialize(args, received_value));
    });

    server_rpc.invoke_client(client_id, 3, 2.718f);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(float_eq(received_value, 2.718f));

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("RPC with multiple arguments") {
    Address server_addr("::1", 34584);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);
    REQUIRE(client_net.local_client_id() != 0);

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    RpcManager client_rpc;
    client_rpc.set_network_manager(&client_net);
    server_net.set_rpc_manager(&server_rpc);
    client_net.set_rpc_manager(&client_rpc);

    int received_x = 0;
    float received_y = 0.0f;
    std::string received_msg;
    server_rpc.register_handler(99, [&received_x, &received_y, &received_msg](ClientId, BitStream& args) {
        REQUIRE(deserialize(args, received_x));
        REQUIRE(deserialize(args, received_y));
        REQUIRE(deserialize(args, received_msg));
    });

    client_rpc.invoke_server(99, 42, 3.14f, std::string("multi-arg"));

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_x == 42);
    REQUIRE(float_eq(received_y, 3.14f));
    REQUIRE(received_msg == "multi-arg");

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Unregistered RPC is silently ignored") {
    Address server_addr("::1", 34585);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);
    REQUIRE(client_net.local_client_id() != 0);

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    RpcManager client_rpc;
    client_rpc.set_network_manager(&client_net);
    server_net.set_rpc_manager(&server_rpc);
    client_net.set_rpc_manager(&client_rpc);

    // Do NOT register handler 77
    bool handler_called = false;
    server_rpc.register_handler(88, [&handler_called](ClientId, BitStream&) {
        handler_called = true;
    });

    // Invoke unregistered RPC
    client_rpc.invoke_server(77, 123);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(!handler_called);

    client_net.disconnect();
    server_net.stop();
}

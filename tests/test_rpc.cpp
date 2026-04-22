#include "campello_net/network_entity.hpp"
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

// ── Mock bridges ────────────────────────────────────────────────────────────

namespace {
struct MockEntityBridge : INetworkEntityBridge {
    std::vector<EntityHandle> destroys;
    EntityHandle next_handle = 1000;

    EntityHandle spawn(PrefabId, NetworkId, const std::vector<std::uint8_t>&) override {
        return next_handle++;
    }
    void destroy(EntityHandle handle) override {
        destroys.push_back(handle);
    }
};
} // namespace

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
    server_rpc.register_handler(7, [&received_value, &received_sender](const RpcParams& params, BitStream& args) {
        received_sender = params.sender;
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
    client_rpc.register_handler(3, [&received_value](const RpcParams& /*params*/, BitStream& args) {
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
    server_rpc.register_handler(99, [&received_x, &received_y, &received_msg](const RpcParams&, BitStream& args) {
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
    server_rpc.register_handler(88, [&handler_called](const RpcParams&, BitStream&) {
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

TEST_CASE("Server broadcasts RPC to all clients") {
    Address server_addr("::1", 34586);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_a;
    client_a.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    NetworkManager client_b;
    client_b.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_a.poll();
        client_b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() == 2 && client_a.local_client_id() != 0 && client_b.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 2);

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);

    RpcManager client_a_rpc;
    client_a_rpc.set_network_manager(&client_a);
    RpcManager client_b_rpc;
    client_b_rpc.set_network_manager(&client_b);

    server_net.set_rpc_manager(&server_rpc);
    client_a.set_rpc_manager(&client_a_rpc);
    client_b.set_rpc_manager(&client_b_rpc);

    int received_a = 0;
    int received_b = 0;
    client_a_rpc.register_handler(10, [&received_a](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received_a));
    });
    client_b_rpc.register_handler(10, [&received_b](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received_b));
    });

    server_rpc.invoke_broadcast(10, 777);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_a.poll();
        client_b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_a == 777);
    REQUIRE(received_b == 777);

    client_a.disconnect();
    client_b.disconnect();
    server_net.stop();
}

TEST_CASE("RpcManager drops oversized RPC payloads") {
    Address server_addr("::1", 34590);

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

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    server_rpc.set_max_payload_size(8); // tiny limit

    RpcManager client_rpc;
    client_rpc.set_network_manager(&client_net);

    server_net.set_rpc_manager(&server_rpc);
    client_net.set_rpc_manager(&client_rpc);

    int received = 0;
    server_rpc.register_handler(20, [&received](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received));
    });

    // Small payload (4 bytes for int32) should pass
    client_rpc.invoke_server(20, 42);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(received == 42);

    // Large payload should be silently dropped
    std::string big_msg(100, 'x');
    client_rpc.invoke_server(20, big_msg);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Value unchanged — oversized RPC was dropped
    REQUIRE(received == 42);

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Server invoke_owner sends RPC only to entity owner") {
    Address server_addr("::1", 34587);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_a;
    client_a.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    NetworkManager client_b;
    client_b.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_a.poll();
        client_b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() == 2 && client_a.local_client_id() != 0 && client_b.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 2);

    // Entity manager on server
    MockEntityBridge entity_bridge;
    NetworkEntityManager entity_mgr;
    entity_mgr.set_bridge(&entity_bridge);
    server_net.set_entity_manager(&entity_mgr);

    NetworkId ent = entity_mgr.spawn(1, {});
    // Set owner to client_a
    ClientId owner_id = client_a.local_client_id();
    entity_mgr.set_owner(ent, owner_id);

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    server_rpc.set_entity_manager(&entity_mgr);

    RpcManager client_a_rpc;
    client_a_rpc.set_network_manager(&client_a);
    RpcManager client_b_rpc;
    client_b_rpc.set_network_manager(&client_b);

    server_net.set_rpc_manager(&server_rpc);
    client_a.set_rpc_manager(&client_a_rpc);
    client_b.set_rpc_manager(&client_b_rpc);

    int received_a = 0;
    int received_b = 0;
    client_a_rpc.register_handler(11, [&received_a](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received_a));
    });
    client_b_rpc.register_handler(11, [&received_b](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received_b));
    });

    server_rpc.invoke_owner(11, ent, 555);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_a.poll();
        client_b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_a == 555);
    REQUIRE(received_b == 0);

    client_a.disconnect();
    client_b.disconnect();
    server_net.stop();
}

TEST_CASE("Server invoke_not_owner excludes entity owner") {
    Address server_addr("::1", 34588);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_a;
    client_a.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    NetworkManager client_b;
    client_b.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_a.poll();
        client_b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() == 2 && client_a.local_client_id() != 0 && client_b.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 2);

    MockEntityBridge entity_bridge;
    NetworkEntityManager entity_mgr;
    entity_mgr.set_bridge(&entity_bridge);
    server_net.set_entity_manager(&entity_mgr);

    NetworkId ent = entity_mgr.spawn(1, {});
    ClientId owner_id = client_a.local_client_id();
    entity_mgr.set_owner(ent, owner_id);

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    server_rpc.set_entity_manager(&entity_mgr);

    RpcManager client_a_rpc;
    client_a_rpc.set_network_manager(&client_a);
    RpcManager client_b_rpc;
    client_b_rpc.set_network_manager(&client_b);

    server_net.set_rpc_manager(&server_rpc);
    client_a.set_rpc_manager(&client_a_rpc);
    client_b.set_rpc_manager(&client_b_rpc);

    int received_a = 0;
    int received_b = 0;
    client_a_rpc.register_handler(12, [&received_a](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received_a));
    });
    client_b_rpc.register_handler(12, [&received_b](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received_b));
    });

    server_rpc.invoke_not_owner(12, ent, 999);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_a.poll();
        client_b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_a == 0);
    REQUIRE(received_b == 999);

    client_a.disconnect();
    client_b.disconnect();
    server_net.stop();
}

TEST_CASE("invoke_owner on server-owned entity does nothing") {
    Address server_addr("::1", 34589);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_a;
    client_a.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_a.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() == 1 && client_a.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);

    MockEntityBridge entity_bridge;
    NetworkEntityManager entity_mgr;
    entity_mgr.set_bridge(&entity_bridge);
    server_net.set_entity_manager(&entity_mgr);

    NetworkId ent = entity_mgr.spawn(1, {});
    // Owner defaults to 0 (server)

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    server_rpc.set_entity_manager(&entity_mgr);

    RpcManager client_a_rpc;
    client_a_rpc.set_network_manager(&client_a);

    server_net.set_rpc_manager(&server_rpc);
    client_a.set_rpc_manager(&client_a_rpc);

    int received_a = 0;
    client_a_rpc.register_handler(13, [&received_a](const RpcParams&, BitStream& args) {
        REQUIRE(deserialize(args, received_a));
    });

    server_rpc.invoke_owner(13, ent, 111);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_a.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_a == 0);

    client_a.disconnect();
    server_net.stop();
}


TEST_CASE("RpcParams carries sender, timestamp and rtt") {
    Address server_addr("::1", 34595);

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

    RpcParams received_params{};
    server_rpc.register_handler(70, [&received_params](const RpcParams& params, BitStream&) {
        received_params = params;
    });

    client_rpc.invoke_server(70);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_params.sender == client_id);
    REQUIRE(received_params.server_timestamp > 0.0);
    // RTT might be 0 on loopback / very fast local connections, so just check non-negative
    REQUIRE(received_params.sender_rtt >= 0.0f);

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("ServerOnly authority rejects client RPCs") {
    Address server_addr("::1", 34596);

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

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    RpcManager client_rpc;
    client_rpc.set_network_manager(&client_net);
    server_net.set_rpc_manager(&server_rpc);
    client_net.set_rpc_manager(&client_rpc);

    int received_anyone = 0;
    int received_server_only = 0;
    server_rpc.register_handler(71, [&received_anyone](const RpcParams&, BitStream&) {
        ++received_anyone;
    }, RpcAuthority::Anyone);
    server_rpc.register_handler(72, [&received_server_only](const RpcParams&, BitStream&) {
        ++received_server_only;
    }, RpcAuthority::ServerOnly);

    // Client invokes both RPCs
    client_rpc.invoke_server(71);
    client_rpc.invoke_server(72);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(received_anyone == 1);
    REQUIRE(received_server_only == 0); // rejected

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Per-RPC rate limit drops excess calls") {
    Address server_addr("::1", 34597);

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

    RpcManager server_rpc;
    server_rpc.set_network_manager(&server_net);
    RpcManager client_rpc;
    client_rpc.set_network_manager(&client_net);
    server_net.set_rpc_manager(&server_rpc);
    client_net.set_rpc_manager(&client_rpc);

    int received = 0;
    server_rpc.register_handler(73, [&received](const RpcParams&, BitStream&) {
        ++received;
    });
    // Allow only 2 RPCs per second, burst of 2
    server_rpc.set_rpc_rate_limit(73, 2.0f, 2.0f);

    // Send 5 RPCs in rapid succession
    for (int i = 0; i < 5; ++i) {
        client_rpc.invoke_server(73);
    }

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Burst of 2 should pass, rest dropped
    REQUIRE(received == 2);

    client_net.disconnect();
    server_net.stop();
}

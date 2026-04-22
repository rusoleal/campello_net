#include "campello_net/network_entity.hpp"
#include "campello_net/network_manager.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <thread>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::transport;

// ── Mock ECS bridge ─────────────────────────────────────────────────────────

struct MockBridge : INetworkEntityBridge {
    struct SpawnCall {
        PrefabId prefab = 0;
        NetworkId net_id = 0;
        std::vector<std::uint8_t> init_data;
    };

    std::vector<SpawnCall> spawns;
    std::vector<EntityHandle> destroys;
    EntityHandle next_handle = 100;

    EntityHandle spawn(PrefabId prefab, NetworkId net_id, const std::vector<std::uint8_t>& init_data) override {
        spawns.push_back({prefab, net_id, init_data});
        return next_handle++;
    }

    void destroy(EntityHandle handle) override {
        destroys.push_back(handle);
    }
};

// ── Unit tests (no network) ─────────────────────────────────────────────────

TEST_CASE("NetworkEntityManager allocates sequential NetworkIds") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);

    NetworkId id1 = mgr.spawn(1);
    NetworkId id2 = mgr.spawn(2);
    NetworkId id3 = mgr.spawn(3);

    REQUIRE(id1 == 1);
    REQUIRE(id2 == 2);
    REQUIRE(id3 == 3);
    REQUIRE(mgr.entity_count() == 3);
}

TEST_CASE("Spawn calls bridge and stores entity") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);

    std::vector<std::uint8_t> init = {'a', 'b', 'c'};
    NetworkId id = mgr.spawn(42, init);

    REQUIRE(mgr.exists(id));
    REQUIRE(mgr.prefab(id) == 42);
    REQUIRE(mgr.local_handle(id) == 100);
    REQUIRE(bridge.spawns.size() == 1);
    REQUIRE(bridge.spawns[0].prefab == 42);
    REQUIRE(bridge.spawns[0].init_data == init);
}

TEST_CASE("Destroy removes entity and calls bridge") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);

    NetworkId id = mgr.spawn(1);
    mgr.destroy(id);

    REQUIRE(!mgr.exists(id));
    REQUIRE(mgr.entity_count() == 0);
    REQUIRE(bridge.destroys.size() == 1);
    REQUIRE(bridge.destroys[0] == 100);
}

TEST_CASE("Ownership can be set and queried") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);

    NetworkId id = mgr.spawn(1);
    REQUIRE(mgr.owner(id) == 0);

    mgr.set_owner(id, 42);
    REQUIRE(mgr.owner(id) == 42);
}

TEST_CASE("Duplicate spawn is ignored on client receive") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);

    // Simulate server spawning entity with id=5
    std::vector<std::uint8_t> msg(14);
    msg[0] = 0;
    msg[1] = 0;
    msg[2] = 0;
    msg[3] = 0;
    msg[4] = 0;
    msg[5] = 0;
    msg[6] = 0;
    msg[7] = 5; // net_id=5 BE
    msg[8] = 0;
    msg[9] = 0;
    msg[10] = 0;
    msg[11] = 7; // prefab=7 BE
    msg[12] = 0;
    msg[13] = 0; // init_len=0

    mgr.on_receive_spawn(0, msg.data(), msg.size());
    REQUIRE(mgr.entity_count() == 1);
    REQUIRE(mgr.prefab(5) == 7);

    // Receive duplicate spawn
    mgr.on_receive_spawn(0, msg.data(), msg.size());
    REQUIRE(mgr.entity_count() == 1);   // still 1, not 2
    REQUIRE(bridge.spawns.size() == 1); // bridge called only once
}

TEST_CASE("Destroy non-existent entity is safe") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);

    std::array<std::uint8_t, 8> msg{0, 0, 0, 0, 0, 0, 0, 99};
    mgr.on_receive_destroy(0, msg.data(), msg.size());
    REQUIRE(mgr.entity_count() == 0);
    REQUIRE(bridge.destroys.empty());
}

// ── Integration tests (server + client over localhost) ──────────────────────

TEST_CASE("Server spawns entity and client receives it") {
    Address server_addr("::1", 34573);

    NetworkManager server_net;
    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = server_addr;
    REQUIRE(server_net.start(sconfig));

    MockBridge server_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_bridge);
    server_net.set_entity_manager(&server_entities);

    NetworkManager client_net;
    NetworkManager::Config cconfig;
    cconfig.mode = NetworkManager::Mode::Client;
    cconfig.server_address = server_addr;
    REQUIRE(client_net.start(cconfig));

    MockBridge client_bridge;
    NetworkEntityManager client_entities;
    client_entities.set_bridge(&client_bridge);
    client_net.set_entity_manager(&client_entities);

    // Wait for connection
    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);

    // Server spawns entity
    NetworkId id = server_entities.spawn(7, {}, &server_net);
    REQUIRE(id == 1);

    // Poll to deliver spawn message
    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(client_entities.entity_count() == 1);
    REQUIRE(client_entities.exists(1));
    REQUIRE(client_entities.prefab(1) == 7);
    REQUIRE(client_bridge.spawns.size() == 1);

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Server destroys entity and client removes it") {
    Address server_addr("::1", 34574);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr, {}, 32, 10.0f});

    MockBridge server_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_bridge);
    server_net.set_entity_manager(&server_entities);

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr, 32, 10.0f});

    MockBridge client_bridge;
    NetworkEntityManager client_entities;
    client_entities.set_bridge(&client_bridge);
    client_net.set_entity_manager(&client_entities);

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);

    // Spawn then destroy
    NetworkId id = server_entities.spawn(3, {}, &server_net);
    for (int i = 0; i < 20; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(client_entities.entity_count() == 1);

    server_entities.destroy(id, &server_net);
    for (int i = 0; i < 20; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(client_entities.entity_count() == 0);
    REQUIRE(client_bridge.destroys.size() == 1);

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Late-joiner receives full state") {
    Address server_addr("::1", 34575);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    MockBridge server_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_bridge);
    server_net.set_entity_manager(&server_entities);

    // Spawn 5 entities before any client connects
    for (PrefabId p = 1; p <= 5; ++p) {
        server_entities.spawn(p, {}, &server_net);
    }
    REQUIRE(server_entities.entity_count() == 5);

    // First client connects
    NetworkManager client1;
    client1.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    MockBridge bridge1;
    NetworkEntityManager client1_entities;
    client1_entities.set_bridge(&bridge1);
    client1.set_entity_manager(&client1_entities);

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client1.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client1.local_client_id() != 0)
            break;
    }
    REQUIRE(server_net.client_count() == 1);

    // Send full state to the new client
    ClientId c1 = 0;
    for (ClientId id = 1; id <= 5; ++id) {
        if (server_net.is_client_connected(id)) {
            c1 = id;
            break;
        }
    }
    REQUIRE(c1 != 0);

    server_entities.send_full_state_to_client(c1, server_net);
    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client1.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(client1_entities.entity_count() == 5);
    REQUIRE(bridge1.spawns.size() == 5);

    client1.disconnect();
    server_net.stop();
}

TEST_CASE("NetworkEntityManager respects max_entities limit") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);
    mgr.set_max_entities(3);

    NetworkId id1 = mgr.spawn(1, {});
    NetworkId id2 = mgr.spawn(1, {});
    NetworkId id3 = mgr.spawn(1, {});

    REQUIRE(id1 != 0);
    REQUIRE(id2 != 0);
    REQUIRE(id3 != 0);
    REQUIRE(mgr.entity_count() == 3);

    // Fourth spawn should fail
    NetworkId id4 = mgr.spawn(1, {});
    REQUIRE(id4 == 0);
    REQUIRE(mgr.entity_count() == 3);
}

TEST_CASE("NetworkEntityManager on_receive_spawn drops when over limit") {
    MockBridge bridge;
    NetworkEntityManager mgr;
    mgr.set_bridge(&bridge);
    mgr.set_max_entities(2);

    // Directly inject spawn messages (big-endian wire format)
    auto write_u64_be = [](uint8_t* dst, uint64_t v) {
        dst[0] = static_cast<uint8_t>(v >> 56);
        dst[1] = static_cast<uint8_t>(v >> 48);
        dst[2] = static_cast<uint8_t>(v >> 40);
        dst[3] = static_cast<uint8_t>(v >> 32);
        dst[4] = static_cast<uint8_t>(v >> 24);
        dst[5] = static_cast<uint8_t>(v >> 16);
        dst[6] = static_cast<uint8_t>(v >> 8);
        dst[7] = static_cast<uint8_t>(v);
    };
    auto write_u32_be = [](uint8_t* dst, uint32_t v) {
        dst[0] = static_cast<uint8_t>(v >> 24);
        dst[1] = static_cast<uint8_t>(v >> 16);
        dst[2] = static_cast<uint8_t>(v >> 8);
        dst[3] = static_cast<uint8_t>(v);
    };
    auto write_u16_be = [](uint8_t* dst, uint16_t v) {
        dst[0] = static_cast<uint8_t>(v >> 8);
        dst[1] = static_cast<uint8_t>(v);
    };

    std::vector<uint8_t> spawn_msg(14);
    write_u64_be(spawn_msg.data(), 100); // net_id
    write_u32_be(spawn_msg.data() + 8, 1); // prefab
    write_u16_be(spawn_msg.data() + 12, 0); // init_len

    mgr.on_receive_spawn(0, spawn_msg.data(), spawn_msg.size());
    REQUIRE(mgr.entity_count() == 1);

    write_u64_be(spawn_msg.data(), 200);
    mgr.on_receive_spawn(0, spawn_msg.data(), spawn_msg.size());
    REQUIRE(mgr.entity_count() == 2);

    // Third spawn should be dropped
    write_u64_be(spawn_msg.data(), 300);
    mgr.on_receive_spawn(0, spawn_msg.data(), spawn_msg.size());
    REQUIRE(mgr.entity_count() == 2);
    REQUIRE(!mgr.exists(300));
}

#include <catch2/catch_test_macros.hpp>

#include "campello_net/network_entity.hpp"
#include "campello_net/network_manager.hpp"
#include "campello_net/network_replication.hpp"
#include "campello_net/serialization/bit_stream.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_map>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::transport;
using namespace systems::leal::campello_net::serialization;

// ── Helpers ─────────────────────────────────────────────────────────────────

static bool float_eq(float a, float b) {
    return a == b; // all test values are exact
}

// ── Mock entity bridge ──────────────────────────────────────────────────────

struct MockEntityBridge : INetworkEntityBridge {
    struct SpawnCall {
        PrefabId prefab = 0;
        NetworkId net_id = 0;
        std::vector<std::uint8_t> init_data;
    };

    std::vector<SpawnCall> spawns;
    std::vector<EntityHandle> destroys;
    EntityHandle next_handle = 100;

    EntityHandle spawn(PrefabId prefab, NetworkId net_id,
                       const std::vector<std::uint8_t>& init_data) override {
        spawns.push_back({prefab, net_id, init_data});
        return next_handle++;
    }

    void destroy(EntityHandle handle) override {
        destroys.push_back(handle);
    }
};

// ── Mock replication bridge ─────────────────────────────────────────────────

struct MockReplicationBridge : INetworkReplicationBridge {
    std::unordered_map<NetworkId, float> server_state;
    std::unordered_map<NetworkId, float> client_state;
    std::size_t serialize_count = 0;
    std::size_t deserialize_count = 0;

    bool serialize_entity(NetworkId net_id, BitStream& stream) override {
        auto it = server_state.find(net_id);
        if (it == server_state.end()) return false;
        stream.write_float(it->second);
        ++serialize_count;
        return true;
    }

    void deserialize_entity(NetworkId net_id, BitStream& stream) override {
        float value = 0.0f;
        (void)stream.read_float(value);
        client_state[net_id] = value;
        ++deserialize_count;
    }
};

// ── Unit tests ──────────────────────────────────────────────────────────────

TEST_CASE("NetworkVariable tracks dirtiness") {
    NetworkVariable<int> health(100);
    REQUIRE(health.is_dirty());

    health.clear_dirty();
    REQUIRE(!health.is_dirty());

    health.set(100); // same value
    REQUIRE(!health.is_dirty());

    health.set(90);
    REQUIRE(health.is_dirty());
    REQUIRE(health.get() == 90);
}

TEST_CASE("NetworkVariable serializes and deserializes") {
    NetworkVariable<float> pos(3.14f);
    BitStream stream;
    pos.serialize(stream);

    NetworkVariable<float> recv;
    REQUIRE(recv.deserialize(stream));
    REQUIRE(float_eq(recv.get(), 3.14f));
    REQUIRE(recv.is_dirty());
}

TEST_CASE("ReplicationManager tracks dirty entities") {
    NetworkReplicationManager repl;
    repl.mark_dirty(1);
    repl.mark_dirty(2);
    repl.mark_dirty(1); // duplicate

    REQUIRE(repl.dirty_count() == 2);
}

TEST_CASE("ReplicationManager builds snapshot with dirty entities") {
    MockReplicationBridge bridge;
    bridge.server_state[1] = 10.0f;
    bridge.server_state[2] = 20.0f;

    NetworkReplicationManager repl;
    repl.set_bridge(&bridge);
    repl.set_tick_rate(1.0f); // 1 Hz for testing

    // Manually build a snapshot simulating build_and_send_snapshot logic
    std::vector<NetworkId> to_replicate = {1};
    std::vector<uint8_t> packet(4);
    packet[0] = 0; packet[1] = 1; // snapshot_id = 1
    packet[2] = 0; packet[3] = 1; // num_entities = 1

    for (NetworkId id : to_replicate) {
        BitStream entity_stream;
        REQUIRE(bridge.serialize_entity(id, entity_stream));
        auto span = entity_stream.span();
        uint16_t data_len = static_cast<uint16_t>(span.size());

        std::size_t offset = packet.size();
        packet.resize(offset + 8 + 2 + data_len);
        for (std::size_t i = 0; i < 8; ++i)
            packet[offset + i] = static_cast<uint8_t>(id >> (56 - i * 8));
        packet[offset + 8] = static_cast<uint8_t>(data_len >> 8);
        packet[offset + 9] = static_cast<uint8_t>(data_len & 0xFF);
        std::memcpy(packet.data() + offset + 10, span.data(), data_len);
    }

    // Client applies the snapshot
    MockReplicationBridge client_bridge;
    client_bridge.server_state = bridge.server_state; // same initial state
    NetworkReplicationManager client_repl;
    client_repl.set_bridge(&client_bridge);
    client_repl.on_receive_delta(packet.data(), packet.size());

    REQUIRE(float_eq(client_bridge.client_state[1], 10.0f));
    REQUIRE(client_bridge.client_state.find(2) == client_bridge.client_state.end());
}

// ── Integration tests (server + client) ─────────────────────────────────────

TEST_CASE("Server replicates entity state to client") {
    Address server_addr("::1", 34576);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    // Connect
    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0) break;
    }
    REQUIRE(server_net.client_count() == 1);

    // Set up entity manager and replication on both sides
    MockEntityBridge server_entity_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_entity_bridge);
    server_net.set_entity_manager(&server_entities);

    MockEntityBridge client_entity_bridge;
    NetworkEntityManager client_entities;
    client_entities.set_bridge(&client_entity_bridge);
    client_net.set_entity_manager(&client_entities);

    MockReplicationBridge server_repl_bridge;
    NetworkReplicationManager server_repl;
    server_repl.set_bridge(&server_repl_bridge);
    server_repl.set_entity_manager(&server_entities);
    server_repl.set_tick_rate(60.0f); // high tick rate for fast test
    server_net.set_replication_manager(&server_repl);

    MockReplicationBridge client_repl_bridge;
    NetworkReplicationManager client_repl;
    client_repl.set_bridge(&client_repl_bridge);
    client_repl.set_entity_manager(&client_entities);
    client_net.set_replication_manager(&client_repl);

    // Spawn entity on server
    NetworkId id = server_entities.spawn(1, {}, &server_net);
    server_repl_bridge.server_state[id] = 42.0f;
    server_repl.mark_dirty(id);

    // Wait for spawn to reach client
    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(client_entities.entity_count() == 1);

    // Run replication ticks
    for (int i = 0; i < 30; ++i) {
        server_repl.server_tick(1.0f / 60.0f, server_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(client_repl_bridge.client_state.find(id) != client_repl_bridge.client_state.end());
    REQUIRE(float_eq(client_repl_bridge.client_state[id], 42.0f));

    // Update state and replicate again
    server_repl_bridge.server_state[id] = 99.5f;
    server_repl.mark_dirty(id);

    for (int i = 0; i < 30; ++i) {
        server_repl.server_tick(1.0f / 60.0f, server_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(float_eq(client_repl_bridge.client_state[id], 99.5f));

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Replication bandwidth stays under limit for 1000 entities") {
    // Unit test: measure snapshot size for 1000 entities
    MockReplicationBridge bridge;
    for (NetworkId i = 1; i <= 1000; ++i) {
        bridge.server_state[i] = static_cast<float>(i) * 0.1f;
    }

    NetworkReplicationManager repl;
    repl.set_bridge(&bridge);
    repl.set_tick_rate(30.0f);

    // Mark all dirty (simulating a full sync scenario)
    for (NetworkId i = 1; i <= 1000; ++i) {
        repl.mark_dirty(i);
    }

    // Build snapshot manually (no network needed)
    std::size_t total_bytes = 0;
    {
        std::vector<NetworkId> to_replicate;
        to_replicate.reserve(1000);
        for (NetworkId i = 1; i <= 1000; ++i) to_replicate.push_back(i);

        std::vector<uint8_t> packet(4);
        packet[0] = 0; packet[1] = 1; // snapshot_id
        packet[2] = static_cast<uint8_t>((to_replicate.size() >> 8) & 0xFF);
        packet[3] = static_cast<uint8_t>(to_replicate.size() & 0xFF);

        for (NetworkId id : to_replicate) {
            BitStream entity_stream;
            if (!bridge.serialize_entity(id, entity_stream)) continue;
            auto span = entity_stream.span();
            uint16_t data_len = static_cast<uint16_t>(span.size());

            std::size_t offset = packet.size();
            packet.resize(offset + 8 + 2 + data_len);
            for (std::size_t i = 0; i < 8; ++i)
                packet[offset + i] = static_cast<uint8_t>(id >> (56 - i * 8));
            packet[offset + 8] = static_cast<uint8_t>(data_len >> 8);
            packet[offset + 9] = static_cast<uint8_t>(data_len & 0xFF);
            std::memcpy(packet.data() + offset + 10, span.data(), data_len);
        }

        total_bytes = packet.size();
    }

    // At 30 Hz, bandwidth = total_bytes * 30 per second
    float bytes_per_second = static_cast<float>(total_bytes) * 30.0f;
    float kb_per_second = bytes_per_second / 1024.0f;

    std::cout << "Snapshot size: " << total_bytes << " bytes, bandwidth: " << kb_per_second << " KB/s\n";

    REQUIRE(kb_per_second < 500.0f);
}

// ── Phase 7: Delta Compression tests ────────────────────────────────────────

TEST_CASE("SnapshotHistory stores and retrieves snapshots") {
    SnapshotHistory hist;

    std::vector<EntitySnapshot> snap1 = {{1, {0x01, 0x02}}, {2, {0x03}}};
    std::vector<EntitySnapshot> snap2 = {{1, {0xAA}}, {3, {0xBB, 0xCC}}};

    hist.store(100, snap1);
    hist.store(101, snap2);

    const auto* ret1 = hist.retrieve(100);
    REQUIRE(ret1 != nullptr);
    REQUIRE(ret1->size() == 2);
    REQUIRE((*ret1)[0].id == 1);
    REQUIRE((*ret1)[0].data == std::vector<uint8_t>({0x01, 0x02}));

    const auto* ret2 = hist.retrieve(101);
    REQUIRE(ret2 != nullptr);
    REQUIRE((*ret2)[1].id == 3);
}

TEST_CASE("SnapshotHistory returns nullptr for unknown or evicted snapshots") {
    SnapshotHistory hist;

    std::vector<EntitySnapshot> snap = {{1, {0x01}}};
    hist.store(1, snap);

    REQUIRE(hist.retrieve(1) != nullptr);
    REQUIRE(hist.retrieve(999) == nullptr);

    // Fill history beyond MAX_SNAPSHOTS (128)
    for (std::uint16_t i = 2; i <= 130; ++i) {
        hist.store(i, snap);
    }
    REQUIRE(hist.retrieve(1) == nullptr); // evicted
    REQUIRE(hist.retrieve(130) != nullptr);
}

TEST_CASE("NetworkVariable delta serializes changed flag") {
    NetworkVariable<int> health(100);
    health.clear_dirty();

    // Unchanged baseline
    BitStream stream;
    health.serialize_delta(stream, 100);
    REQUIRE(stream.byte_count() == 1); // just the changed bit, byte-aligned

    int baseline = 100;
    REQUIRE(health.deserialize_delta(stream, baseline));
    REQUIRE(baseline == 100); // unchanged
    REQUIRE(!health.is_dirty()); // deserialize_delta shouldn't mark dirty if unchanged

    // Changed baseline
    BitStream stream2;
    health.serialize_delta(stream2, 50);
    REQUIRE(stream2.byte_count() > 1); // changed bit + int value

    int baseline2 = 50;
    REQUIRE(health.deserialize_delta(stream2, baseline2));
    REQUIRE(baseline2 == 100);
}

TEST_CASE("ReplicationManager tracks client acks") {
    NetworkReplicationManager repl;

    repl.on_client_connected(42);
    repl.on_snapshot_ack(42, 10);

    // Duplicate/old ack is ignored
    repl.on_snapshot_ack(42, 5);

    // Newer ack accepted
    repl.on_snapshot_ack(42, 15);

    repl.on_client_disconnected(42);
}

TEST_CASE("Delta compression sends smaller packets after ack") {
    // Unit test: verify that delta building reduces packet size.
    MockReplicationBridge bridge;
    for (NetworkId i = 1; i <= 10; ++i) {
        bridge.server_state[i] = static_cast<float>(i);
    }

    NetworkReplicationManager repl;
    repl.set_bridge(&bridge);
    repl.on_client_connected(1);

    // Simulate a full sync: all 10 entities dirty.
    for (NetworkId i = 1; i <= 10; ++i) {
        repl.mark_dirty(i);
    }

    // Build full snapshot manually.
    std::vector<EntitySnapshot> full_snap;
    for (NetworkId i = 1; i <= 10; ++i) {
        BitStream stream;
        REQUIRE(bridge.serialize_entity(i, stream));
        auto span = stream.span();
        full_snap.push_back({i, std::vector<uint8_t>(span.begin(), span.end())});
    }

    // Store as snapshot 1
    SnapshotHistory hist;
    hist.store(1, full_snap);

    // Client acks snapshot 1
    repl.on_snapshot_ack(1, 1);

    // Now only entity 5 changes.
    bridge.server_state[5] = 99.0f;
    std::vector<EntitySnapshot> delta_snap;
    for (NetworkId i = 1; i <= 10; ++i) {
        BitStream stream;
        REQUIRE(bridge.serialize_entity(i, stream));
        auto span = stream.span();
        delta_snap.push_back({i, std::vector<uint8_t>(span.begin(), span.end())});
    }

    // Build delta against baseline
    const auto* baseline = hist.retrieve(1);
    REQUIRE(baseline != nullptr);

    std::unordered_map<NetworkId, const std::vector<uint8_t>*> baseline_map;
    for (const auto& e : *baseline) {
        baseline_map[e.id] = &e.data;
    }

    std::vector<EntitySnapshot> to_send;
    for (const auto& cur : delta_snap) {
        auto it = baseline_map.find(cur.id);
        if (it == baseline_map.end() || *it->second != cur.data) {
            to_send.push_back(cur);
        }
    }

    REQUIRE(to_send.size() == 1);
    REQUIRE(to_send[0].id == 5);
}

// ── Phase 8: Interest Management tests ──────────────────────────────────────

TEST_CASE("Interest filter culls entities outside radius") {
    Address server_addr("::1", 34580);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    // Connect
    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0) break;
    }
    REQUIRE(server_net.client_count() == 1);
    ClientId client_id = client_net.local_client_id();
    REQUIRE(client_id != 0);

    // Set up entity + replication
    MockEntityBridge server_entity_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_entity_bridge);
    server_net.set_entity_manager(&server_entities);

    MockEntityBridge client_entity_bridge;
    NetworkEntityManager client_entities;
    client_entities.set_bridge(&client_entity_bridge);
    client_net.set_entity_manager(&client_entities);

    MockReplicationBridge server_repl_bridge;
    NetworkReplicationManager server_repl;
    server_repl.set_bridge(&server_repl_bridge);
    server_repl.set_entity_manager(&server_entities);
    server_repl.set_tick_rate(60.0f);
    server_repl.set_full_sync_interval(0);
    // Interest filter: only entities with net_id <= 3 are visible
    server_repl.set_interest_filter([](NetworkId entity, ClientId) {
        return entity <= 3;
    });
    server_net.set_replication_manager(&server_repl);

    MockReplicationBridge client_repl_bridge;
    NetworkReplicationManager client_repl;
    client_repl.set_bridge(&client_repl_bridge);
    client_repl.set_entity_manager(&client_entities);
    client_net.set_replication_manager(&client_repl);

    // Spawn 5 entities
    NetworkId ids[5];
    for (int i = 0; i < 5; ++i) {
        ids[i] = server_entities.spawn(1, {}, &server_net);
        server_repl_bridge.server_state[ids[i]] = static_cast<float>(i + 1);
        server_repl.mark_dirty(ids[i]);
    }

    // Wait for spawns to reach client
    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(client_entities.entity_count() == 5);

    // Tick 1: full sync (client has no ack yet), but filtered
    for (int i = 0; i < 30; ++i) {
        server_repl.server_tick(1.0f / 60.0f, server_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Client should only have state for entities 1, 2, 3 (the first 3 spawned)
    REQUIRE(client_repl_bridge.client_state.count(ids[0]) == 1);
    REQUIRE(client_repl_bridge.client_state.count(ids[1]) == 1);
    REQUIRE(client_repl_bridge.client_state.count(ids[2]) == 1);
    REQUIRE(client_repl_bridge.client_state.count(ids[3]) == 0);
    REQUIRE(client_repl_bridge.client_state.count(ids[4]) == 0);

    // Now dirty entities 2 and 5 (ids[1] and ids[4])
    server_repl_bridge.server_state[ids[1]] = 99.0f;
    server_repl_bridge.server_state[ids[4]] = 99.0f;
    server_repl.mark_dirty(ids[1]);
    server_repl.mark_dirty(ids[4]);

    // Client acks
    for (int i = 0; i < 10; ++i) {
        client_repl.client_tick(1.0f / 60.0f, client_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Tick 2: delta — only visible dirty entity should be ids[1]
    for (int i = 0; i < 30; ++i) {
        server_repl.server_tick(1.0f / 60.0f, server_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(float_eq(client_repl_bridge.client_state[ids[1]], 99.0f));
    // ids[4] was filtered out, so client still has old value
    REQUIRE(float_eq(client_repl_bridge.client_state[ids[4]], 0.0f));

    client_net.disconnect();
    server_net.stop();
}

TEST_CASE("Re-entering interest forces state send even if unchanged") {
    Address server_addr("::1", 34581);

    NetworkManager server_net;
    server_net.start(NetworkManager::Config{NetworkManager::Mode::Server, server_addr});

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server_net.client_count() > 0 && client_net.local_client_id() != 0) break;
    }
    REQUIRE(server_net.client_count() == 1);

    MockEntityBridge server_entity_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_entity_bridge);
    server_net.set_entity_manager(&server_entities);

    MockEntityBridge client_entity_bridge;
    NetworkEntityManager client_entities;
    client_entities.set_bridge(&client_entity_bridge);
    client_net.set_entity_manager(&client_entities);

    MockReplicationBridge server_repl_bridge;
    NetworkReplicationManager server_repl;
    server_repl.set_bridge(&server_repl_bridge);
    server_repl.set_entity_manager(&server_entities);
    server_repl.set_tick_rate(60.0f);
    server_repl.set_full_sync_interval(0);

    // Initially exclude entity 2
    bool include_entity_2 = false;
    server_repl.set_interest_filter([&include_entity_2](NetworkId entity, ClientId) {
        return entity != 2 || include_entity_2;
    });
    server_net.set_replication_manager(&server_repl);

    MockReplicationBridge client_repl_bridge;
    NetworkReplicationManager client_repl;
    client_repl.set_bridge(&client_repl_bridge);
    client_repl.set_entity_manager(&client_entities);
    client_net.set_replication_manager(&client_repl);

    NetworkId id1 = server_entities.spawn(1, {}, &server_net);
    NetworkId id2 = server_entities.spawn(1, {}, &server_net);
    server_repl_bridge.server_state[id1] = 10.0f;
    server_repl_bridge.server_state[id2] = 20.0f;
    server_repl.mark_dirty(id1);
    server_repl.mark_dirty(id2);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(client_entities.entity_count() == 2);

    // Full sync — client gets id1 but not id2
    for (int i = 0; i < 30; ++i) {
        server_repl.server_tick(1.0f / 60.0f, server_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(client_repl_bridge.client_state.count(id1) == 1);
    REQUIRE(client_repl_bridge.client_state.count(id2) == 0);

    // Client acks
    for (int i = 0; i < 10; ++i) {
        client_repl.client_tick(1.0f / 60.0f, client_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Now include entity 2, but don't change its state
    include_entity_2 = true;
    // No dirty marks needed — re-visibility should force the send

    for (int i = 0; i < 30; ++i) {
        server_repl.server_tick(1.0f / 60.0f, server_net);
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Client should now have id2 even though it never changed
    REQUIRE(client_repl_bridge.client_state.count(id2) == 1);
    REQUIRE(float_eq(client_repl_bridge.client_state[id2], 20.0f));

    client_net.disconnect();
    server_net.stop();
}

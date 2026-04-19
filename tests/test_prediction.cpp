#include "campello_net/network_entity.hpp"
#include "campello_net/network_manager.hpp"
#include "campello_net/network_replication.hpp"
#include "campello_net/prediction/input_buffer.hpp"
#include "campello_net/prediction/lag_compensator.hpp"
#include "campello_net/rpc_manager.hpp"
#include "campello_net/serialization/bit_stream.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::transport;
using namespace systems::leal::campello_net::serialization;

// ── Unit tests: InputBuffer ─────────────────────────────────────────────────

TEST_CASE("InputBuffer stores and retrieves inputs") {
    InputBuffer buf;

    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    buf.store(42, 10, data);

    std::vector<uint8_t> out;
    REQUIRE(buf.retrieve(42, 10, out));
    REQUIRE(out == data);

    REQUIRE(buf.has(42, 10));
    REQUIRE(!buf.has(42, 11));
    REQUIRE(!buf.has(99, 10));
}

TEST_CASE("InputBuffer overwrites old ticks on wrap") {
    InputBuffer buf;

    std::vector<uint8_t> data_a = {0xAA};
    std::vector<uint8_t> data_b = {0xBB};

    buf.store(1, 0, data_a);
    REQUIRE(buf.has(1, 0));

    // Wrap around MAX_TICKS (256)
    buf.store(1, 256, data_b);
    REQUIRE(buf.has(1, 256));
    REQUIRE(!buf.has(1, 0)); // overwritten
}

TEST_CASE("InputBuffer prunes old entries") {
    InputBuffer buf;

    buf.store(1, 10, std::vector<uint8_t>{0x01});
    buf.store(1, 20, std::vector<uint8_t>{0x02});
    buf.store(2, 15, std::vector<uint8_t>{0x03});

    buf.prune_up_to(15);

    REQUIRE(!buf.has(1, 10));
    REQUIRE(buf.has(1, 20));
    REQUIRE(!buf.has(2, 15));
}

TEST_CASE("InputBuffer clear_client removes all data") {
    InputBuffer buf;

    buf.store(1, 10, std::vector<uint8_t>{0x01});
    buf.store(2, 10, std::vector<uint8_t>{0x02});

    buf.clear_client(1);

    REQUIRE(!buf.has(1, 10));
    REQUIRE(buf.has(2, 10));
}

TEST_CASE("InputBuffer tracks last received tick") {
    InputBuffer buf;

    REQUIRE(buf.last_received_tick(1) == 0);

    buf.store(1, 5, std::vector<uint8_t>{0x01});
    buf.store(1, 100, std::vector<uint8_t>{0x02});
    buf.store(1, 50, std::vector<uint8_t>{0x03});

    REQUIRE(buf.last_received_tick(1) == 100);
}

// ── Unit tests: ReplicationManager prediction mode ──────────────────────────

struct PredictionTestBridge : INetworkReplicationBridge {
    std::unordered_map<NetworkId, float> client_state;

    bool serialize_entity(NetworkId, BitStream&) override {
        return true;
    }
    void deserialize_entity(NetworkId net_id, BitStream& stream) override {
        float val = 0.0f;
        (void)stream.read_float(val);
        client_state[net_id] = val;
    }
};

TEST_CASE("Prediction mode routes snapshots to callback instead of bridge") {
    PredictionTestBridge bridge;
    NetworkReplicationManager repl;
    repl.set_bridge(&bridge);

    std::vector<NetworkId> received_ids;
    repl.set_prediction_mode(true);
    repl.set_snapshot_received_callback([&received_ids](NetworkId id, BitStream&) {
        received_ids.push_back(id);
    });

    // Build a fake delta packet with 2 entities
    std::vector<uint8_t> packet(4);
    packet[0] = 0;
    packet[1] = 1; // snapshot_id = 1
    packet[2] = 0;
    packet[3] = 2; // num_entities = 2

    for (NetworkId id : {10, 20}) {
        BitStream es;
        es.write_float(1.0f);
        auto span = es.span();
        std::size_t offset = packet.size();
        packet.resize(offset + 10 + span.size());
        for (std::size_t i = 0; i < 8; ++i)
            packet[offset + i] = static_cast<uint8_t>(id >> (56 - i * 8));
        packet[offset + 8] = static_cast<uint8_t>(span.size() >> 8);
        packet[offset + 9] = static_cast<uint8_t>(span.size() & 0xFF);
        std::memcpy(packet.data() + offset + 10, span.data(), span.size());
    }

    repl.on_receive_delta(packet.data(), packet.size());

    REQUIRE(received_ids.size() == 2);
    REQUIRE(received_ids[0] == 10);
    REQUIRE(received_ids[1] == 20);

    // Bridge should NOT have received anything
    REQUIRE(bridge.client_state.empty());
}

TEST_CASE("Non-prediction mode applies snapshots via bridge") {
    PredictionTestBridge bridge;
    NetworkReplicationManager repl;
    repl.set_bridge(&bridge);

    // Build a fake delta packet
    std::vector<uint8_t> packet(4);
    packet[0] = 0;
    packet[1] = 1;
    packet[2] = 0;
    packet[3] = 1;

    NetworkId id = 99;
    BitStream es;
    es.write_float(42.0f);
    auto span = es.span();
    std::size_t offset = packet.size();
    packet.resize(offset + 10 + span.size());
    for (std::size_t i = 0; i < 8; ++i)
        packet[offset + i] = static_cast<uint8_t>(id >> (56 - i * 8));
    packet[offset + 8] = static_cast<uint8_t>(span.size() >> 8);
    packet[offset + 9] = static_cast<uint8_t>(span.size() & 0xFF);
    std::memcpy(packet.data() + offset + 10, span.data(), span.size());

    repl.on_receive_delta(packet.data(), packet.size());

    REQUIRE(bridge.client_state.count(99) == 1);
    REQUIRE(bridge.client_state[99] == 42.0f);
}

// ── Integration test: client sends inputs via RPC ───────────────────────────

TEST_CASE("Client sends ticked inputs to server via RPC") {
    Address server_addr("::1", 34586);

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

    InputBuffer input_buffer;

    // Server registers handler for input RPC (id 50)
    server_rpc.register_handler(50, [&input_buffer](ClientId sender, BitStream& args) {
        std::uint16_t tick = 0;
        float move_x = 0.0f;
        float move_y = 0.0f;
        REQUIRE(deserialize(args, tick));
        REQUIRE(deserialize(args, move_x));
        REQUIRE(deserialize(args, move_y));

        // Serialize input data into a blob for the buffer
        BitStream input_stream;
        serialize(input_stream, move_x);
        serialize(input_stream, move_y);
        auto span = input_stream.span();
        input_buffer.store(sender, tick, std::span<const uint8_t>(span.data(), span.size()));
    });

    // Client sends 3 inputs
    client_rpc.invoke_server(50, static_cast<std::uint16_t>(1), 1.0f, 0.0f);
    client_rpc.invoke_server(50, static_cast<std::uint16_t>(2), 0.5f, 0.5f);
    client_rpc.invoke_server(50, static_cast<std::uint16_t>(3), -1.0f, 0.0f);

    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ClientId client_id = client_net.local_client_id();
    REQUIRE(input_buffer.has(client_id, 1));
    REQUIRE(input_buffer.has(client_id, 2));
    REQUIRE(input_buffer.has(client_id, 3));
    REQUIRE(!input_buffer.has(client_id, 4));

    // Verify retrieved data
    std::vector<uint8_t> out;
    REQUIRE(input_buffer.retrieve(client_id, 2, out));
    BitStream verify_stream(std::span<const uint8_t>(out.data(), out.size()));
    float vx = 0.0f, vy = 0.0f;
    REQUIRE(verify_stream.read_float(vx));
    REQUIRE(verify_stream.read_float(vy));
    REQUIRE(vx == 0.5f);
    REQUIRE(vy == 0.5f);

    client_net.disconnect();
    server_net.stop();
}

// ── Unit tests: LagCompensator ──────────────────────────────────────────────

TEST_CASE("LagCompensator calculates rewind tick from RTT") {
    LagCompensator comp;
    comp.set_tick_rate(30.0f); // 33.33 ms per tick

    // RTT = 0 ms → no rewind
    REQUIRE(comp.get_rewind_tick(100, 0.0f) == 100);

    // RTT = 100 ms → one-way = 50 ms → ~1.5 ticks → round to 2
    REQUIRE(comp.get_rewind_tick(100, 100.0f) == 98);

    // RTT = 66.6 ms → one-way = 33.3 ms → ~1 tick
    REQUIRE(comp.get_rewind_tick(100, 66.6f) == 99);

    // RTT = 200 ms → one-way = 100 ms → ~3 ticks
    REQUIRE(comp.get_rewind_tick(100, 200.0f) == 97);

    // Clamps to 0
    REQUIRE(comp.get_rewind_tick(1, 200.0f) == 0);
}

TEST_CASE("LagCompensator retrieves entity state from snapshot history") {
    SnapshotHistory hist;

    // Store snapshot 10 with two entities
    std::vector<EntitySnapshot> snap10 = {
        {1, {0x01, 0x02}},
        {2, {0xAA, 0xBB, 0xCC}},
    };
    hist.store(10, snap10);

    // Store snapshot 11 with one entity changed
    std::vector<EntitySnapshot> snap11 = {
        {1, {0x03}},
        {2, {0xAA, 0xBB, 0xCC}},
    };
    hist.store(11, snap11);

    LagCompensator comp;
    comp.set_snapshot_history(&hist);

    std::vector<uint8_t> out;

    // Retrieve entity 1 at tick 10
    REQUIRE(comp.get_entity_state(10, 1, out));
    REQUIRE(out == std::vector<uint8_t>({0x01, 0x02}));

    // Retrieve entity 1 at tick 11
    REQUIRE(comp.get_entity_state(11, 1, out));
    REQUIRE(out == std::vector<uint8_t>({0x03}));

    // Missing tick
    REQUIRE(!comp.get_entity_state(99, 1, out));

    // Missing entity
    REQUIRE(!comp.get_entity_state(10, 99, out));

    // Retrieve all entities at tick 10
    std::vector<EntitySnapshot> all;
    REQUIRE(comp.get_all_entities(10, all));
    REQUIRE(all.size() == 2);
    REQUIRE(all[0].id == 1);
    REQUIRE(all[1].id == 2);
}

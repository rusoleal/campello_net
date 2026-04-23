#include <campello_net/network_entity.hpp>
#include <campello_net/network_manager.hpp>
#include <campello_net/network_replication.hpp>
#include <campello_net/serialization/bit_stream.hpp>
#include <campello_net/transport/loopback_transport.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <thread>

#ifdef __APPLE__
#include <malloc/malloc.h>
#elif defined(_WIN32) && defined(_MSC_VER)
#include <crtdbg.h>
#else
#include <malloc.h>
#endif

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::serialization;
using namespace systems::leal::campello_net::transport;

// ── Cross-platform allocation counter ────────────────────────────────────────
//
// macOS: malloc_zone_statistics (bytes in use)
// Linux: mallinfo2 (bytes in use)
// Windows (MSVC): _CrtMemState (debug heap blocks)
// Other: no-op (tests run but do not validate allocation-free behaviour)

struct MallocCounter {
#ifdef __APPLE__
    malloc_statistics_t before{};
#elif defined(_WIN32) && defined(_MSC_VER)
    _CrtMemState before{};
#else
    mallinfo2 before{};
#endif

    MallocCounter() {
        reset();
    }

    void reset() {
#ifdef __APPLE__
        malloc_zone_statistics(malloc_default_zone(), &before);
#elif defined(_WIN32) && defined(_MSC_VER)
        _CrtMemCheckpoint(&before);
#else
        before = mallinfo2();
#endif
    }

    [[nodiscard]] std::size_t bytes_allocated() const {
#ifdef __APPLE__
        malloc_statistics_t after{};
        malloc_zone_statistics(malloc_default_zone(), &after);
        return static_cast<std::size_t>(after.size_in_use > before.size_in_use ? after.size_in_use - before.size_in_use
                                                                               : 0);
#elif defined(_WIN32) && defined(_MSC_VER)
        _CrtMemState after{};
        _CrtMemCheckpoint(&after);
        _CrtMemState diff{};
        _CrtMemDifference(&diff, &before, &after);
        return static_cast<std::size_t>(diff.lSizes[_NORMAL_BLOCK]);
#else
        mallinfo2 after = mallinfo2();
        return static_cast<std::size_t>(after.uordblks > before.uordblks ? after.uordblks - before.uordblks : 0);
#endif
    }
};

// ── Minimal bridge for replication tests ─────────────────────────────────────

struct AuditBridge : public INetworkEntityBridge, public INetworkReplicationBridge {
    std::unordered_map<NetworkId, float> state;

    EntityHandle spawn(PrefabId /*prefab*/, NetworkId net_id, const std::vector<std::uint8_t>& /*init_data*/) override {
        state[net_id] = 0.0f;
        return static_cast<EntityHandle>(net_id);
    }

    void destroy(EntityHandle handle) override {
        state.erase(static_cast<NetworkId>(handle));
    }

    bool serialize_entity(NetworkId net_id, BitStream& stream) override {
        auto it = state.find(net_id);
        if (it == state.end())
            return false;
        stream.write_float(it->second);
        return true;
    }

    void deserialize_entity(NetworkId net_id, BitStream& stream) override {
        float v = 0.0f;
        stream.read_float(v);
        state[net_id] = v;
    }
};

// ── Helpers ──────────────────────────────────────────────────────────────────

static void connect_or_timeout(NetworkManager& server, NetworkManager& client, int timeout_ms = 1000) {
    for (int i = 0; i < timeout_ms / 8; ++i) {
        server.poll();
        client.poll();
        if (server.client_count() > 0 && client.local_client_id() != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

// ── Tests ────────────────────────────────────────────────────────────────────

TEST_CASE("NetworkManager::poll() does not allocate on steady state") {
    Address server_addr("::1", 34590);

    NetworkManager::Config server_cfg;
    server_cfg.mode = NetworkManager::Mode::Server;
    server_cfg.bind_address = server_addr;
    server_cfg.max_messages_per_sec = 0.0f;
    server_cfg.max_bytes_per_sec = 0.0f;
    server_cfg.max_rpcs_per_sec = 0.0f;

    NetworkManager server_net;
    server_net.start(server_cfg);

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    connect_or_timeout(server_net, client_net);
    REQUIRE(server_net.client_count() == 1);

    MallocCounter counter;
    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        client_net.poll();
    }
    std::size_t allocs = counter.bytes_allocated();
    INFO("Allocations during 60 poll() iterations: " << allocs);
    REQUIRE(allocs == 0);
}

TEST_CASE("server_tick() does not allocate when no state changes") {
    Address server_addr("::1", 34591);

    NetworkManager::Config server_cfg;
    server_cfg.mode = NetworkManager::Mode::Server;
    server_cfg.bind_address = server_addr;
    server_cfg.max_messages_per_sec = 0.0f;
    server_cfg.max_bytes_per_sec = 0.0f;
    server_cfg.max_rpcs_per_sec = 0.0f;

    NetworkManager server_net;
    server_net.start(server_cfg);

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    connect_or_timeout(server_net, client_net);

    AuditBridge server_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_bridge);
    server_net.set_entity_manager(&server_entities);

    NetworkReplicationManager server_repl;
    server_repl.set_bridge(&server_bridge);
    server_repl.set_entity_manager(&server_entities);
    server_repl.set_tick_rate(60.0f);
    server_net.set_replication_manager(&server_repl);

    AuditBridge client_bridge;
    NetworkEntityManager client_entities;
    client_entities.set_bridge(&client_bridge);
    client_net.set_entity_manager(&client_entities);

    NetworkReplicationManager client_repl;
    client_repl.set_bridge(&client_bridge);
    client_repl.set_entity_manager(&client_entities);
    client_repl.set_tick_rate(60.0f);
    client_net.set_replication_manager(&client_repl);

    MallocCounter counter;
    for (int i = 0; i < 30; ++i) {
        server_repl.server_tick(1.0f / 60.0f, server_net);
    }
    std::size_t allocs = counter.bytes_allocated();
    INFO("Allocations during 30 idle server_tick() calls: " << allocs);
    REQUIRE(allocs == 0);
}

TEST_CASE("server_tick() does not allocate with dirty entities") {
    Address server_addr("::1", 34592);

    NetworkManager::Config server_cfg;
    server_cfg.mode = NetworkManager::Mode::Server;
    server_cfg.bind_address = server_addr;
    server_cfg.max_messages_per_sec = 0.0f;
    server_cfg.max_bytes_per_sec = 0.0f;
    server_cfg.max_rpcs_per_sec = 0.0f;

    NetworkManager server_net;
    server_net.start(server_cfg);

    NetworkManager client_net;
    client_net.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr});

    connect_or_timeout(server_net, client_net);

    AuditBridge server_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_bridge);
    server_net.set_entity_manager(&server_entities);

    NetworkReplicationManager server_repl;
    server_repl.set_bridge(&server_bridge);
    server_repl.set_entity_manager(&server_entities);
    server_repl.set_tick_rate(60.0f);
    server_net.set_replication_manager(&server_repl);

    AuditBridge client_bridge;
    NetworkEntityManager client_entities;
    client_entities.set_bridge(&client_bridge);
    client_net.set_entity_manager(&client_entities);

    NetworkReplicationManager client_repl;
    client_repl.set_bridge(&client_bridge);
    client_repl.set_entity_manager(&client_entities);
    client_repl.set_tick_rate(60.0f);
    client_net.set_replication_manager(&client_repl);

    // Spawn and dirty 50 entities (warm-up, allocation allowed)
    std::vector<NetworkId> ids;
    for (int i = 0; i < 50; ++i) {
        NetworkId id = server_entities.spawn(1, {}, &server_net);
        REQUIRE(id != 0);
        ids.push_back(id);
        server_bridge.state[id] = static_cast<float>(i);
        server_repl.mark_dirty(id);
    }

    // Run one tick to warm up the replication pipeline
    server_repl.server_tick(1.0f / 60.0f, server_net);
    for (int i = 0; i < 30; ++i) {
        server_net.poll();
        client_net.poll();
        client_repl.client_tick(1.0f / 60.0f, client_net);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Warm-up: cycle through all 128 history slots so each slot's blob has capacity.
    for (int w = 0; w < 130; ++w) {
        for (NetworkId id : ids) {
            server_bridge.state[id] += 1.0f;
            server_repl.mark_dirty(id);
        }
        server_repl.server_tick(1.0f / 60.0f, server_net);
        server_net.poll();
        client_net.poll();
        client_repl.client_tick(1.0f / 60.0f, client_net);
    }

    // Now measure allocations during replication ticks with dirty state
    MallocCounter counter;
    for (int i = 0; i < 30; ++i) {
        for (NetworkId id : ids) {
            server_bridge.state[id] += 1.0f;
            server_repl.mark_dirty(id);
        }
        server_repl.server_tick(1.0f / 60.0f, server_net);
    }
    std::size_t allocs = counter.bytes_allocated();
    INFO("Allocations during 30 dirty server_tick() calls with 50 entities: " << allocs);
    REQUIRE(allocs == 0);
}

TEST_CASE("BitStream write/read primitives do not allocate after warm-up") {
    // BitStream uses an internal std::vector that grows on first use.
    // After warm-up, clear() retains capacity and subsequent writes are allocation-free.
    BitStream stream;
    // Warm-up: force the internal vector to grow to its steady-state capacity
    for (int i = 0; i < 10; ++i) {
        stream.write_bool(true);
        stream.write_int8(42);
        stream.write_int16(1234);
        stream.write_int32(static_cast<std::int32_t>(0xDEADBEEF));
        stream.write_float(3.14f);
        stream.write_double(2.718);
        stream.write_string("hello");
        stream.write_varint(static_cast<std::uint64_t>(i));
        stream.reset();
    }

    MallocCounter counter;
    for (int i = 0; i < 1000; ++i) {
        stream.write_bool(true);
        stream.write_int8(42);
        stream.write_int16(1234);
        stream.write_int32(static_cast<std::int32_t>(0xDEADBEEF));
        stream.write_float(3.14f);
        stream.write_double(2.718);
        stream.write_string("hello");
        stream.write_varint(static_cast<std::uint64_t>(i));

        stream.reset_read();
        bool b = false;
        std::int8_t i8 = 0;
        std::int16_t i16 = 0;
        std::int32_t i32 = 0;
        float f = 0.0f;
        double d = 0.0;
        std::string s;
        std::uint64_t v = 0;
        stream.read_bool(b);
        stream.read_int8(i8);
        stream.read_int16(i16);
        stream.read_int32(i32);
        stream.read_float(f);
        stream.read_double(d);
        stream.read_string(s);
        stream.read_varint(v);
        stream.reset();
    }
    std::size_t allocs = counter.bytes_allocated();
    INFO("Allocations during 1000 BitStream round-trips: " << allocs);
    REQUIRE(allocs == 0);
}

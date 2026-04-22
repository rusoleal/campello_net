#include <algorithm>
#include <campello_net/net_stats.hpp>
#include <campello_net/network_entity.hpp>
#include <campello_net/network_manager.hpp>
#include <campello_net/network_replication.hpp>
#include <campello_net/serialization/bit_stream.hpp>
#include <campello_net/transport/address.hpp>
#include <campello_net/transport/loopback_transport.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::serialization;
using namespace systems::leal::campello_net::transport;

// ── Simple command-line parser ───────────────────────────────────────────────

struct Args {
    std::size_t entities = 1000;
    std::size_t clients = 4;
    float duration_sec = 10.0f;
    float tick_rate = 30.0f;
    float packet_loss = 0.0f;
    float latency_sec = 0.0f;
    float interest_radius = 0.0f; ///< 0 = replicate all entities
    bool realtime = false;
};

static Args parse_args(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--entities" || arg == "-e") && i + 1 < argc) {
            a.entities = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if ((arg == "--clients" || arg == "-c") && i + 1 < argc) {
            a.clients = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if ((arg == "--duration" || arg == "-d") && i + 1 < argc) {
            a.duration_sec = std::stof(argv[++i]);
        } else if ((arg == "--tick-rate" || arg == "-t") && i + 1 < argc) {
            a.tick_rate = std::stof(argv[++i]);
        } else if ((arg == "--packet-loss" || arg == "-p") && i + 1 < argc) {
            a.packet_loss = std::stof(argv[++i]);
        } else if ((arg == "--latency") && i + 1 < argc) {
            a.latency_sec = std::stof(argv[++i]);
        } else if ((arg == "--interest-radius") && i + 1 < argc) {
            a.interest_radius = std::stof(argv[++i]);
        } else if (arg == "--realtime") {
            a.realtime = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
                R"(campello_net stress test

Usage: stress_test_example [options]

Options:
  -e, --entities N          Number of entities to spawn (default: 1000)
  -c, --clients N           Number of clients to connect (default: 4)
  -d, --duration SEC        Benchmark duration in seconds (default: 10)
  -t, --tick-rate HZ        Replication tick rate (default: 30)
  -p, --packet-loss RATIO   Artificial packet loss [0,1] (default: 0)
      --latency SEC         Artificial latency in seconds (default: 0)
      --interest-radius M   Spatial culling radius; 0 = off (default: 0)
      --realtime            Sleep between ticks instead of free-run
  -h, --help                Show this help

Examples:
  # Baseline: 1000 entities, 4 clients, 10 seconds
  ./stress_test_example

  # Heavy load: 10k entities, 8 clients
  ./stress_test_example -e 10000 -c 8 -d 5

  # With network impairment
  ./stress_test_example -e 5000 -c 4 -p 0.05 --latency 0.05
)";
            std::exit(0);
        }
    }
    return a;
}

// ── Bridge implementation ────────────────────────────────────────────────────

struct EntityState {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

class StressBridge : public INetworkEntityBridge, public INetworkReplicationBridge {
public:
    std::unordered_map<NetworkId, EntityState> state;

    // INetworkEntityBridge
    EntityHandle spawn(PrefabId /*prefab*/, NetworkId net_id, const std::vector<std::uint8_t>& /*init_data*/) override {
        state[net_id] = {};
        return static_cast<EntityHandle>(net_id);
    }

    void destroy(EntityHandle handle) override {
        state.erase(static_cast<NetworkId>(handle));
    }

    // INetworkReplicationBridge
    bool serialize_entity(NetworkId net_id, BitStream& stream) override {
        auto it = state.find(net_id);
        if (it == state.end())
            return false;
        stream.write_float(it->second.x);
        stream.write_float(it->second.y);
        stream.write_float(it->second.z);
        return true;
    }

    void deserialize_entity(NetworkId net_id, BitStream& stream) override {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        stream.read_float(x);
        stream.read_float(y);
        stream.read_float(z);
        state[net_id] = {x, y, z};
    }
};

// ── Benchmark harness ────────────────────────────────────────────────────────

struct TickSample {
    double server_tick_us = 0.0;
    double client_tick_us = 0.0;
    double poll_us = 0.0;
};

struct BenchmarkResult {
    std::size_t entities = 0;
    std::size_t clients = 0;
    float duration_sec = 0.0f;
    float tick_rate = 0.0f;
    std::size_t ticks = 0;

    // CPU
    double total_server_time_sec = 0.0;
    double total_client_time_sec = 0.0;
    double total_poll_time_sec = 0.0;
    double max_tick_time_sec = 0.0;

    // Bandwidth (from NetStats)
    float total_server_bw_out = 0.0f; ///< Sum across all clients (bytes/sec)
    float avg_per_client_bw_out = 0.0f;
    float max_per_client_bw_out = 0.0f;
    std::uint64_t total_bytes_sent = 0;
    std::uint64_t total_packets_sent = 0;

    // Replication
    std::size_t client_entities_received = 0;
};

static BenchmarkResult run_benchmark(const Args& args) {
    const Address server_addr("127.0.0.1", 7777);
    const float dt = 1.0f / args.tick_rate;
    const std::size_t total_ticks = static_cast<std::size_t>(args.duration_sec * args.tick_rate);

    BenchmarkResult result;
    result.entities = args.entities;
    result.clients = args.clients;
    result.duration_sec = args.duration_sec;
    result.tick_rate = args.tick_rate;

    auto hub = std::make_shared<LoopbackHub>();

    // ── Server ──
    NetworkManager server_net;
    auto* server_transport_raw = new LoopbackTransport(hub);
    std::unique_ptr<LoopbackTransport> server_transport(server_transport_raw);
    server_net.set_transport(std::move(server_transport));
    NetworkManager::Config server_cfg;
    server_cfg.mode = NetworkManager::Mode::Server;
    server_cfg.bind_address = server_addr;
    server_cfg.max_messages_per_sec = 0.0f; // Unlimited for benchmark
    server_cfg.max_bytes_per_sec = 0.0f;
    server_cfg.max_rpcs_per_sec = 0.0f;
    server_net.start(server_cfg);

    StressBridge server_bridge;
    NetworkEntityManager server_entities;
    server_entities.set_bridge(&server_bridge);
    server_net.set_entity_manager(&server_entities);

    NetworkReplicationManager server_repl;
    server_repl.set_bridge(&server_bridge);
    server_repl.set_entity_manager(&server_entities);
    server_repl.set_tick_rate(args.tick_rate);
    server_net.set_replication_manager(&server_repl);

    // Interest filter (optional)
    if (args.interest_radius > 0.0f) {
        server_repl.set_interest_filter([radius = args.interest_radius, &bridge = server_bridge](NetworkId entity,
                                                                                                 ClientId /*client*/) {
            auto it = bridge.state.find(entity);
            if (it == bridge.state.end())
                return false;
            const float d2 = it->second.x * it->second.x + it->second.y * it->second.y + it->second.z * it->second.z;
            return d2 <= radius * radius;
        });
    }

    // Spawn entities on a grid with deterministic motion parameters
    for (std::size_t i = 0; i < args.entities; ++i) {
        NetworkId id = server_entities.spawn(1, {}, &server_net);
        if (id == 0) {
            std::cerr << "Failed to spawn entity " << i << " (limit reached)\n";
            break;
        }
        // Initialise position so they are spread out
        float angle = static_cast<float>(i) * 0.01f;
        float dist = static_cast<float>(i) * 2.0f;
        server_bridge.state[id].x = std::cos(angle) * dist;
        server_bridge.state[id].y = std::sin(angle) * dist;
        server_bridge.state[id].z = static_cast<float>(i % 100) * 10.0f;
        server_repl.mark_dirty(id);
    }

    const std::size_t actual_entities = server_entities.entity_count();
    result.entities = actual_entities;

    // ── Clients ──
    std::vector<std::unique_ptr<NetworkManager>> clients;
    std::vector<std::unique_ptr<StressBridge>> client_bridges;
    std::vector<std::unique_ptr<NetworkEntityManager>> client_entity_mgrs;
    std::vector<std::unique_ptr<NetworkReplicationManager>> client_repls;
    std::vector<ClientId> server_client_ids;

    server_net.on_client_connected([&](ClientId id) {
        server_client_ids.push_back(id);
        server_entities.send_full_state_to_client(id, server_net);
    });

    std::vector<LoopbackTransport*> client_transport_ptrs;
    for (std::size_t i = 0; i < args.clients; ++i) {
        auto client = std::make_unique<NetworkManager>();
        auto* client_transport_raw = new LoopbackTransport(hub);
        client_transport_ptrs.push_back(client_transport_raw);
        std::unique_ptr<LoopbackTransport> client_transport(client_transport_raw);
        client->set_transport(std::move(client_transport));
        NetworkManager::Config client_cfg;
        client_cfg.mode = NetworkManager::Mode::Client;
        client_cfg.server_address = server_addr;
        client_cfg.max_messages_per_sec = 0.0f; // Unlimited for benchmark
        client_cfg.max_bytes_per_sec = 0.0f;
        client_cfg.max_rpcs_per_sec = 0.0f;
        client->start(client_cfg);

        auto bridge = std::make_unique<StressBridge>();
        auto entity_mgr = std::make_unique<NetworkEntityManager>();
        entity_mgr->set_bridge(bridge.get());
        client->set_entity_manager(entity_mgr.get());

        auto repl = std::make_unique<NetworkReplicationManager>();
        repl->set_bridge(bridge.get());
        repl->set_entity_manager(entity_mgr.get());
        repl->set_tick_rate(args.tick_rate);
        client->set_replication_manager(repl.get());

        clients.push_back(std::move(client));
        client_bridges.push_back(std::move(bridge));
        client_entity_mgrs.push_back(std::move(entity_mgr));
        client_repls.push_back(std::move(repl));
    }

    // ── Connection handshake ──
    constexpr int max_connect_polls = 200;
    int connect_polls = 0;
    for (; connect_polls < max_connect_polls; ++connect_polls) {
        server_net.poll();
        for (auto& c : clients)
            c->poll();
        if (server_net.client_count() == args.clients)
            break;
        if (args.realtime)
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (server_net.client_count() != args.clients) {
        std::cerr << "Warning: only " << server_net.client_count() << " of " << args.clients << " clients connected.\n";
    }

    // Wait a bit longer for late-joiner catch-up
    for (int i = 0; i < 60; ++i) {
        server_net.poll();
        for (auto& c : clients)
            c->poll();
        if (args.realtime)
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // Apply network impairment after handshake is complete
    server_transport_raw->set_packet_loss(args.packet_loss);
    server_transport_raw->set_latency(args.latency_sec);
    for (auto* t : client_transport_ptrs) {
        t->set_packet_loss(args.packet_loss);
        t->set_latency(args.latency_sec);
    }

    // ── Benchmark loop ──
    using Clock = std::chrono::steady_clock;
    std::vector<TickSample> samples;
    samples.reserve(total_ticks);

    for (std::size_t tick = 0; tick < total_ticks; ++tick) {
        // Update entity positions deterministically
        const float t = static_cast<float>(tick) * dt;
        for (auto& [id, ent] : server_bridge.state) {
            float angle = static_cast<float>(id) * 0.1f + t * 2.0f;
            float radius = static_cast<float>(id) * 2.0f;
            ent.x = std::cos(angle) * radius;
            ent.y = std::sin(angle) * radius;
            ent.z = std::sin(angle * 0.5f) * 100.0f;
            server_repl.mark_dirty(id);
        }

        TickSample sample;

        // Server tick
        auto t0 = Clock::now();
        server_repl.server_tick(dt, server_net);
        auto t1 = Clock::now();
        sample.server_tick_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        // Poll all managers
        t0 = Clock::now();
        server_net.poll();
        for (auto& c : clients)
            c->poll();
        t1 = Clock::now();
        sample.poll_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        // Client ack ticks
        t0 = Clock::now();
        for (std::size_t i = 0; i < clients.size(); ++i) {
            client_repls[i]->client_tick(dt, *clients[i]);
        }
        t1 = Clock::now();
        sample.client_tick_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        samples.push_back(sample);
        result.max_tick_time_sec =
            std::max(result.max_tick_time_sec, (sample.server_tick_us + sample.poll_us + sample.client_tick_us) * 1e-6);

        if (args.realtime) {
            auto elapsed = std::chrono::duration<float>(Clock::now() - t0).count();
            float sleep_sec = dt - elapsed;
            if (sleep_sec > 0.0f) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(static_cast<std::int64_t>(sleep_sec * 1'000'000)));
            }
        }
    }

    // ── Gather results ──
    result.ticks = total_ticks;
    result.total_server_time_sec =
        std::accumulate(samples.begin(), samples.end(), 0.0, [](double s, const TickSample& ts) {
            return s + ts.server_tick_us * 1e-6;
        });
    result.total_poll_time_sec =
        std::accumulate(samples.begin(), samples.end(), 0.0, [](double s, const TickSample& ts) {
            return s + ts.poll_us * 1e-6;
        });
    result.total_client_time_sec =
        std::accumulate(samples.begin(), samples.end(), 0.0, [](double s, const TickSample& ts) {
            return s + ts.client_tick_us * 1e-6;
        });

    for (ClientId cid : server_client_ids) {
        NetStats stats = server_net.net_stats(cid);
        result.total_server_bw_out += stats.bandwidth_out;
        result.max_per_client_bw_out = std::max(result.max_per_client_bw_out, stats.bandwidth_out);
        result.total_bytes_sent += stats.bytes_sent;
        result.total_packets_sent += stats.packets_sent;
    }

    if (!server_client_ids.empty()) {
        result.avg_per_client_bw_out = result.total_server_bw_out / static_cast<float>(server_client_ids.size());
    }

    // Count how many entities each client received
    for (const auto& bridge : client_bridges) {
        result.client_entities_received += bridge->state.size();
    }

    return result;
}

// ── Pretty printer ───────────────────────────────────────────────────────────

static std::string fmt_float(double v, int prec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(prec) << v;
    return oss.str();
}
static std::string fmt_float(float v, int prec) {
    return fmt_float(static_cast<double>(v), prec);
}

static void print_results(const BenchmarkResult& r) {
    const double avg_server_us = (r.total_server_time_sec / static_cast<double>(r.ticks)) * 1e6;
    const double avg_poll_us = (r.total_poll_time_sec / static_cast<double>(r.ticks)) * 1e6;
    const double avg_client_us = (r.total_client_time_sec / static_cast<double>(r.ticks)) * 1e6;
    const double total_cpu_us = avg_server_us + avg_poll_us + avg_client_us;

    auto row = [](const std::string& label, const std::string& value) {
        std::cout << "║ " << std::left << std::setw(48) << label << std::right << std::setw(13) << value << " ║\n";
    };

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                campello_net Stress Test Results                      ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Configuration                                                        ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    row("Entities:", std::to_string(r.entities));
    row("Clients:", std::to_string(r.clients));
    row("Duration:", fmt_float(r.duration_sec, 1) + " s");
    row("Tick rate:", std::to_string(static_cast<int>(r.tick_rate)) + " Hz");
    row("Ticks executed:", std::to_string(r.ticks));
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Server CPU per tick                                                  ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    row("  Replication tick:", fmt_float(avg_server_us, 1) + " us");
    row("  Network poll:", fmt_float(avg_poll_us, 1) + " us");
    row("  Client acks:", fmt_float(avg_client_us, 1) + " us");
    row("  Total:", fmt_float(total_cpu_us, 1) + " us");
    row("  Max tick:", fmt_float(r.max_tick_time_sec * 1e6, 1) + " us");
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Bandwidth (server -> clients)                                        ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    row("  Total outbound:", fmt_float(r.total_server_bw_out / 1024.0f, 1) + " KiB/s");
    row("  Avg per client:", fmt_float(r.avg_per_client_bw_out / 1024.0f, 1) + " KiB/s");
    row("  Max per client:", fmt_float(r.max_per_client_bw_out / 1024.0f, 1) + " KiB/s");
    row("  Total bytes sent:", std::to_string(r.total_bytes_sent));
    row("  Total packets:", std::to_string(r.total_packets_sent));
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Client state                                                         ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════════╣\n";
    row("  Total entities received:", std::to_string(r.client_entities_received));
    if (r.clients > 0) {
        float avg = static_cast<float>(r.client_entities_received) / static_cast<float>(r.clients);
        row("  Avg entities per client:", fmt_float(avg, 0));
    }
    std::cout << "╚══════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
}

// ── Entry point ──────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    Args args = parse_args(argc, argv);

    std::cout << "campello_net stress test\n";
    std::cout << "  Entities:     " << args.entities << "\n";
    std::cout << "  Clients:      " << args.clients << "\n";
    std::cout << "  Duration:     " << args.duration_sec << " s\n";
    std::cout << "  Tick rate:    " << args.tick_rate << " Hz\n";
    std::cout << "  Packet loss:  " << args.packet_loss * 100.0f << "%\n";
    std::cout << "  Latency:      " << args.latency_sec * 1000.0f << " ms\n";
    if (args.interest_radius > 0.0f) {
        std::cout << "  Interest radius: " << args.interest_radius << " m\n";
    } else {
        std::cout << "  Interest:     off (all entities to all clients)\n";
    }
    std::cout << "  Mode:         " << (args.realtime ? "realtime" : "free-run (CPU-bound)") << "\n";
    std::cout << "\n";

    BenchmarkResult result = run_benchmark(args);
    print_results(result);

    return 0;
}

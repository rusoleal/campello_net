#include <atomic>
#include <campello_net/network_entity.hpp>
#include <campello_net/network_manager.hpp>
#include <campello_net/network_replication.hpp>
#include <campello_net/rpc_manager.hpp>
#include <campello_net/serialization/bit_stream.hpp>
#include <campello_net/serialization/serializable.hpp>
#include <campello_net/transport/address.hpp>
#include <campello_net/version.hpp>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
#include <conio.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::serialization;
using namespace systems::leal::campello_net::transport;

// ── Constants ────────────────────────────────────────────────────────────────

constexpr float WORLD_W = 76.0f;
constexpr float WORLD_H = 22.0f;
constexpr float SPEED = 25.0f;
constexpr float TICK_RATE = 20.0f;
constexpr float DT = 1.0f / TICK_RATE;
constexpr std::size_t NUM_AI_CUBES = 10;
constexpr NetworkId AI_CUBE_BASE_ID = 1;
constexpr NetworkId PLAYER_CUBE_BASE_ID = 100;
constexpr std::uint16_t RPC_INPUT = 1;

// ── Shared state ─────────────────────────────────────────────────────────────

struct CubeState {
    float x = 0.0f;
    float y = 0.0f;
};

static std::atomic<bool> g_running{true};

static void on_sigint(int) {
    g_running = false;
}

// ── Non-blocking stdin ───────────────────────────────────────────────────────

#ifdef _WIN32
static void set_stdin_nonblocking(bool) {
    // No-op: _kbhit() handles non-blocking checks on Windows.
}

static bool try_read_char(char& out) {
    if (_kbhit()) {
        out = static_cast<char>(_getch());
        return true;
    }
    return false;
}
#else
static void set_stdin_nonblocking(bool nonblocking) {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0)
        return;
    if (nonblocking) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    } else {
        fcntl(STDIN_FILENO, F_SETFL, flags & ~static_cast<int>(O_NONBLOCK));
    }
}

static bool try_read_char(char& out) {
    char ch = 0;
    if (::read(STDIN_FILENO, &ch, 1) == 1) {
        out = ch;
        return true;
    }
    return false;
}
#endif

// ── Bridge ───────────────────────────────────────────────────────────────────

class CubeBridge : public INetworkEntityBridge, public INetworkReplicationBridge {
public:
    std::unordered_map<NetworkId, CubeState> state;

    EntityHandle spawn(PrefabId /*prefab*/, NetworkId net_id, const std::vector<std::uint8_t>& /*init_data*/) override {
        state[net_id] = {};
        return static_cast<EntityHandle>(net_id);
    }

    void destroy(EntityHandle handle) override {
        state.erase(static_cast<NetworkId>(handle));
    }

    bool serialize_entity(NetworkId net_id, BitStream& stream) override {
        auto it = state.find(net_id);
        if (it == state.end())
            return false;
        stream.write_float(it->second.x);
        stream.write_float(it->second.y);
        return true;
    }

    void deserialize_entity(NetworkId net_id, BitStream& stream) override {
        float x = 0.0f, y = 0.0f;
        stream.read_float(x);
        stream.read_float(y);
        state[net_id] = {x, y};
    }

    void interpolate_entity(NetworkId net_id, BitStream& older, BitStream& newer, float t) override {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        older.read_float(x0);
        older.read_float(y0);
        newer.read_float(x1);
        newer.read_float(y1);
        state[net_id].x = x0 + (x1 - x0) * t;
        state[net_id].y = y0 + (y1 - y0) * t;
    }
};

// ── ANSI helpers ─────────────────────────────────────────────────────────────

static void clear_screen() {
    std::cout << "\033[2J\033[H";
}

static void hide_cursor() {
    std::cout << "\033[?25l";
}

static void show_cursor() {
    std::cout << "\033[?25h";
}

// ── Server ───────────────────────────────────────────────────────────────────

struct PlayerInput {
    std::int8_t dx = 0;
    std::int8_t dy = 0;
};

class CubeServer {
public:
    explicit CubeServer(std::uint16_t port) {
        net_.on_client_connected([this](ClientId id) {
            on_client_connected(id);
        });
        net_.on_client_disconnected([this](ClientId id) {
            on_client_disconnected(id);
        });

        rpc_.set_network_manager(&net_);
        rpc_.register_handler(RPC_INPUT, [this](const RpcParams& params, BitStream& args) {
            std::int8_t dx = 0, dy = 0;
            deserialize(args, dx);
            deserialize(args, dy);
            player_inputs_[params.sender] = {dx, dy};
        });
        net_.set_rpc_manager(&rpc_);

        entities_.set_bridge(&bridge_);
        net_.set_entity_manager(&entities_);

        repl_.set_bridge(&bridge_);
        repl_.set_entity_manager(&entities_);
        repl_.set_tick_rate(TICK_RATE);
        net_.set_replication_manager(&repl_);

        transport::Address bind_addr("::", port);
        NetworkManager::Config cfg;
        cfg.mode = NetworkManager::Mode::Server;
        cfg.bind_address = bind_addr;
        cfg.max_messages_per_sec = 0.0f;
        cfg.max_bytes_per_sec = 0.0f;
        cfg.max_rpcs_per_sec = 0.0f;
        if (!net_.start(cfg)) {
            std::cerr << "Failed to start server on port " << port << "\n";
            std::exit(1);
        }

        // Spawn AI cubes
        for (std::size_t i = 0; i < NUM_AI_CUBES; ++i) {
            NetworkId id = AI_CUBE_BASE_ID + i;
            entities_.spawn(static_cast<PrefabId>(id), {}, &net_);
            ai_phase_[id] = static_cast<float>(i) * 0.6f;
            bridge_.state[id].x = WORLD_W * 0.5f;
            bridge_.state[id].y = WORLD_H * 0.5f;
        }

        std::cout << "campello_net cubes server on port " << port << "\n";
        std::cout << "Press Ctrl+C to stop.\n";
    }

    void run() {
        auto last = std::chrono::steady_clock::now();
        while (g_running) {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            net_.poll();

            accumulator_ += dt;
            while (accumulator_ >= DT) {
                accumulator_ -= DT;
                tick();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        net_.stop();
    }

private:
    void on_client_connected(ClientId client) {
        NetworkId id = PLAYER_CUBE_BASE_ID + client;
        entities_.spawn(static_cast<PrefabId>(id), {}, &net_);
        bridge_.state[id].x = WORLD_W * 0.5f;
        bridge_.state[id].y = WORLD_H * 0.5f;
        player_entities_[client] = id;
        player_inputs_[client] = {0, 0};
        std::cout << "Player " << client << " joined (cube " << id << ")\n";
    }

    void on_client_disconnected(ClientId client) {
        auto it = player_entities_.find(client);
        if (it != player_entities_.end()) {
            entities_.destroy(it->second, &net_);
            player_entities_.erase(it);
        }
        player_inputs_.erase(client);
        std::cout << "Player " << client << " left\n";
    }

    void tick() {
        // Update AI cubes (circular motion)
        for (std::size_t i = 0; i < NUM_AI_CUBES; ++i) {
            NetworkId id = AI_CUBE_BASE_ID + i;
            ai_phase_[id] += DT * 1.5f;
            float radius = 8.0f + static_cast<float>(i) * 1.5f;
            bridge_.state[id].x = WORLD_W * 0.5f + std::cos(ai_phase_[id]) * radius;
            bridge_.state[id].y = WORLD_H * 0.5f + std::sin(ai_phase_[id]) * radius;
            repl_.mark_dirty(id);
        }

        // Update player cubes from inputs
        for (auto& [client, id] : player_entities_) {
            auto it = player_inputs_.find(client);
            if (it != player_inputs_.end()) {
                bridge_.state[id].x += it->second.dx * SPEED * DT;
                bridge_.state[id].y += it->second.dy * SPEED * DT;
            }
            // Clamp to world
            bridge_.state[id].x = std::clamp(bridge_.state[id].x, 1.0f, WORLD_W - 2.0f);
            bridge_.state[id].y = std::clamp(bridge_.state[id].y, 1.0f, WORLD_H - 2.0f);
            repl_.mark_dirty(id);
        }

        repl_.server_tick(DT, net_);
    }

    NetworkManager net_;
    RpcManager rpc_;
    CubeBridge bridge_; // Must outlive entities_ and repl_
    NetworkEntityManager entities_;
    NetworkReplicationManager repl_;
    float accumulator_ = 0.0f;

    std::unordered_map<ClientId, NetworkId> player_entities_;
    std::unordered_map<ClientId, PlayerInput> player_inputs_;
    std::unordered_map<NetworkId, float> ai_phase_;
};

// ── Client ───────────────────────────────────────────────────────────────────

class CubeClient {
public:
    CubeClient(const std::string& host, std::uint16_t port, std::string name) : name_(std::move(name)) {
        rpc_.set_network_manager(&net_);
        net_.set_rpc_manager(&rpc_);

        entities_.set_bridge(&bridge_);
        net_.set_entity_manager(&entities_);

        repl_.set_bridge(&bridge_);
        repl_.set_entity_manager(&entities_);
        repl_.set_tick_rate(TICK_RATE);
        repl_.set_interpolation_enabled(true);
        repl_.set_interpolation_delay(0.05f);
        net_.set_replication_manager(&repl_);

        transport::Address server_addr(host, port);
        NetworkManager::Config cfg;
        cfg.mode = NetworkManager::Mode::Client;
        cfg.server_address = server_addr;
        if (!net_.start(cfg)) {
            std::cerr << "Failed to connect to " << host << ":" << port << "\n";
            std::exit(1);
        }

        std::cout << "Connecting to " << host << ":" << port << " as '" << name_ << "'...\n";
        for (int i = 0; i < 120; ++i) {
            net_.poll();
            if (net_.local_client_id() != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        if (net_.local_client_id() == 0) {
            std::cerr << "Connection timed out.\n";
            std::exit(1);
        }

        hide_cursor();
        set_stdin_nonblocking(true);
        std::cout << "Connected! Use WASD to move. Press Q to quit.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    ~CubeClient() {
        set_stdin_nonblocking(false);
        show_cursor();
    }

    void run() {
        auto last = std::chrono::steady_clock::now();
        float render_time = 0.0f;

        while (g_running) {
            auto now = std::chrono::steady_clock::now();
            float frame_dt = std::chrono::duration<float>(now - last).count();
            last = now;
            render_time += frame_dt;

            net_.poll();
            repl_.client_tick(frame_dt, net_);
            repl_.client_interpolate(render_time);

            read_input();
            predict_local_player(frame_dt);
            render();

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        net_.stop();
    }

private:
    void read_input() {
        char ch = 0;
        while (try_read_char(ch)) {
            switch (ch) {
            case 'w':
            case 'W':
                input_.dy = -1;
                break;
            case 's':
            case 'S':
                input_.dy = 1;
                break;
            case 'a':
            case 'A':
                input_.dx = -1;
                break;
            case 'd':
            case 'D':
                input_.dx = 1;
                break;
            case 'q':
            case 'Q':
                g_running = false;
                return;
            }
        }

        // Simple key-release simulation: if no new key pressed this frame,
        // check if stdin is empty and clear movement. This is crude but works
        // for a demo. A real game would track key-up events.
        // For this demo, we require holding the key; releasing stops movement.
        // Since we can't detect key-up with non-blocking raw stdin easily,
        // we auto-clear movement after a short timeout.
        if (input_.dx != 0 || input_.dy != 0) {
            input_timeout_ -= frame_dt_accum_;
            if (input_timeout_ <= 0.0f) {
                input_.dx = 0;
                input_.dy = 0;
            }
        }
        frame_dt_accum_ = 0.0f;
    }

    void predict_local_player(float dt) {
        frame_dt_accum_ += dt;
        input_timeout_ = 0.15f; // 150ms of auto-move before stopping

        // Find our player entity
        if (my_entity_id_ == 0) {
            NetworkId expected = PLAYER_CUBE_BASE_ID + net_.local_client_id();
            if (bridge_.state.find(expected) != bridge_.state.end()) {
                my_entity_id_ = expected;
            }
        }

        if (my_entity_id_ == 0)
            return;

        // Predict local movement
        predicted_.x += input_.dx * SPEED * dt;
        predicted_.y += input_.dy * SPEED * dt;
        predicted_.x = std::clamp(predicted_.x, 1.0f, WORLD_W - 2.0f);
        predicted_.y = std::clamp(predicted_.y, 1.0f, WORLD_H - 2.0f);

        // Send input to server
        input_accum_ += dt;
        if (input_accum_ >= DT) {
            input_accum_ -= DT;
            rpc_.invoke_server(RPC_INPUT, input_.dx, input_.dy);
        }
    }

    void render() {
        clear_screen();

        // Top border
        std::cout << "+";
        for (int x = 0; x < static_cast<int>(WORLD_W); ++x)
            std::cout << "-";
        std::cout << "+  campello_net cubes | " << name_ << "\n";

        // Build grid
        int gw = static_cast<int>(WORLD_W);
        int gh = static_cast<int>(WORLD_H);
        std::vector<std::string> grid(static_cast<std::size_t>(gh), std::string(static_cast<std::size_t>(gw), ' '));

        // Draw entities
        for (const auto& [id, st] : bridge_.state) {
            int gx = static_cast<int>(std::round(st.x));
            int gy = static_cast<int>(std::round(st.y));
            if (gx < 0 || gx >= gw || gy < 0 || gy >= gh)
                continue;

            char c = 'o';
            if (id == my_entity_id_) {
                c = '@';
                // Override with predicted position for local player
                int px = static_cast<int>(std::round(predicted_.x));
                int py = static_cast<int>(std::round(predicted_.y));
                if (px >= 0 && px < gw && py >= 0 && py < gh) {
                    grid[static_cast<std::size_t>(py)][static_cast<std::size_t>(px)] = c;
                    continue;
                }
            }
            if (grid[static_cast<std::size_t>(gy)][static_cast<std::size_t>(gx)] == ' ')
                grid[static_cast<std::size_t>(gy)][static_cast<std::size_t>(gx)] = c;
        }

        // Draw grid rows
        for (int y = 0; y < gh; ++y) {
            std::cout << "|" << grid[static_cast<std::size_t>(y)] << "|\n";
        }

        // Bottom border
        std::cout << "+";
        for (int x = 0; x < gw; ++x)
            std::cout << "-";
        std::cout << "+  Entities: " << bridge_.state.size() << " | Input: " << static_cast<int>(input_.dx) << ","
                  << static_cast<int>(input_.dy) << " | Q=quit\n";
    }

    NetworkManager net_;
    RpcManager rpc_;
    CubeBridge bridge_; // Must outlive entities_ and repl_
    NetworkEntityManager entities_;
    NetworkReplicationManager repl_;
    std::string name_;

    NetworkId my_entity_id_ = 0;
    CubeState predicted_;
    PlayerInput input_{};
    float input_timeout_ = 0.0f;
    float frame_dt_accum_ = 0.0f;
    float input_accum_ = 0.0f;
};

// ── Entry point ──────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cout << "campello_net cubes example " << version_string() << "\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " --server <port>\n";
    std::cout << "  " << prog << " --client <host> <port> <name>\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << prog << " --server 7777\n";
    std::cout << "  " << prog << " --client 127.0.0.1 7777 alice\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "--server" || mode == "-s") {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        CubeServer server(port);
        server.run();
    } else if (mode == "--client" || mode == "-c") {
        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }
        std::string host = argv[2];
        std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argv[3]));
        std::string name = argv[4];
        CubeClient client(host, port, name);
        client.run();
    } else {
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}

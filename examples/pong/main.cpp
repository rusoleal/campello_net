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
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::serialization;
using namespace systems::leal::campello_net::transport;

// ── Constants ────────────────────────────────────────────────────────────────

constexpr float WORLD_W = 60.0f;
constexpr float WORLD_H = 20.0f;
constexpr float PADDLE_H = 3.0f;
constexpr float PADDLE_SPEED = 25.0f;
constexpr float BALL_SPEED = 18.0f;
constexpr float TICK_RATE = 20.0f;
constexpr float DT = 1.0f / TICK_RATE;

constexpr std::uint16_t RPC_PADDLE_INPUT = 1;
constexpr std::uint16_t RPC_ASSIGN_PADDLE = 2;
constexpr PrefabId PREFAB_BALL = 1;
constexpr PrefabId PREFAB_SCORE = 2;
constexpr PrefabId PREFAB_PADDLE_BASE = 100;

// ── Shared state ─────────────────────────────────────────────────────────────

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

// ── Game state ───────────────────────────────────────────────────────────────

struct BallState {
    float x = WORLD_W / 2.0f;
    float y = WORLD_H / 2.0f;
    float vx = BALL_SPEED;
    float vy = BALL_SPEED * 0.6f;
};

struct ScoreState {
    std::uint8_t p1 = 0;
    std::uint8_t p2 = 0;
};

// ── Bridge ───────────────────────────────────────────────────────────────────

class PongBridge : public INetworkEntityBridge, public INetworkReplicationBridge {
public:
    std::unordered_map<NetworkId, float> paddle_y; // paddle centre Y by actual NetId
    std::unordered_map<NetworkId, PrefabId> entity_prefab; // track prefab per net_id
    BallState ball;
    bool has_ball = false;
    ScoreState score;

    EntityHandle spawn(PrefabId prefab, NetworkId net_id,
                       const std::vector<std::uint8_t>& /*init_data*/) override {
        if (prefab == PREFAB_BALL) {
            ball_id_ = net_id;
            has_ball = true;
            ball = BallState{};
        } else if (prefab == PREFAB_SCORE) {
            score_id_ = net_id;
            score = ScoreState{};
        } else {
            paddle_y[net_id] = WORLD_H / 2.0f;
        }
        entity_prefab[net_id] = prefab;
        return static_cast<EntityHandle>(net_id);
    }

    void destroy(EntityHandle handle) override {
        NetworkId id = static_cast<NetworkId>(handle);
        paddle_y.erase(id);
        if (ball_id_ == id) {
            has_ball = false;
            ball_id_ = 0;
        }
        if (score_id_ == id) {
            score_id_ = 0;
        }
    }

    bool serialize_entity(NetworkId net_id, BitStream& stream) override {
        if (net_id == ball_id_ && has_ball) {
            stream.write_float(ball.x);
            stream.write_float(ball.y);
            stream.write_float(ball.vx);
            stream.write_float(ball.vy);
            return true;
        }
        if (net_id == score_id_) {
            stream.write_uint8(score.p1);
            stream.write_uint8(score.p2);
            return true;
        }
        auto it = paddle_y.find(net_id);
        if (it == paddle_y.end())
            return false;
        stream.write_float(it->second);
        return true;
    }

    void deserialize_entity(NetworkId net_id, BitStream& stream) override {
        if (net_id == ball_id_) {
            stream.read_float(ball.x);
            stream.read_float(ball.y);
            stream.read_float(ball.vx);
            stream.read_float(ball.vy);
            has_ball = true;
        } else if (net_id == score_id_) {
            std::uint8_t p1 = 0, p2 = 0;
            stream.read_uint8(p1);
            stream.read_uint8(p2);
            score = {p1, p2};
        } else {
            float y = 0.0f;
            stream.read_float(y);
            paddle_y[net_id] = y;
        }
    }

    void interpolate_entity(NetworkId net_id, BitStream& older, BitStream& newer,
                            float t) override {
        if (net_id == ball_id_ && has_ball) {
            BallState b0{}, b1{};
            older.read_float(b0.x);
            older.read_float(b0.y);
            older.read_float(b0.vx);
            older.read_float(b0.vy);
            newer.read_float(b1.x);
            newer.read_float(b1.y);
            newer.read_float(b1.vx);
            newer.read_float(b1.vy);
            ball.x = b0.x + (b1.x - b0.x) * t;
            ball.y = b0.y + (b1.y - b0.y) * t;
            ball.vx = b1.vx;
            ball.vy = b1.vy;
        } else if (net_id == score_id_) {
            deserialize_entity(net_id, newer);
        } else {
            float y0 = 0.0f, y1 = 0.0f;
            older.read_float(y0);
            newer.read_float(y1);
            paddle_y[net_id] = y0 + (y1 - y0) * t;
        }
    }

    // Server-side: mark which net_id is the ball / score
    NetworkId ball_id_ = 0;
    NetworkId score_id_ = 0;
};

// ── Server ───────────────────────────────────────────────────────────────────

class PongServer {
public:
    explicit PongServer(std::uint16_t port) {
        net_.on_client_connected([this](ClientId id) { on_client_connected(id); });
        net_.on_client_disconnected([this](ClientId id) { on_client_disconnected(id); });

        rpc_.set_network_manager(&net_);
        rpc_.register_handler(RPC_PADDLE_INPUT, [this](const RpcParams& params, BitStream& args) {
            std::int8_t dy = 0;
            deserialize(args, dy);
            player_inputs_[params.sender] = dy;
        });
        net_.set_rpc_manager(&rpc_);

        entities_.set_bridge(&bridge_);
        net_.set_entity_manager(&entities_);

        repl_.set_bridge(&bridge_);
        repl_.set_entity_manager(&entities_);
        repl_.set_tick_rate(TICK_RATE);
        net_.set_replication_manager(&repl_);

        NetworkManager::Config cfg;
        cfg.mode = NetworkManager::Mode::Server;
        cfg.bind_address = transport::Address(port);
        cfg.max_clients = 2;
        if (!net_.start(cfg)) {
            std::cerr << "Failed to start server on port " << port << "\n";
            std::exit(1);
        }

        std::cout << "campello_net pong server on port " << port << "\n";
        std::cout << "Waiting for 2 players...\n";
    }

    void run() {
        auto last = std::chrono::steady_clock::now();
        while (g_running) {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            net_.poll();
            repl_.server_tick(dt, net_);

            accumulator_ += dt;
            while (accumulator_ >= DT) {
                accumulator_ -= DT;
                game_tick();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        net_.stop();
    }

private:
    void on_client_connected(ClientId id) {
        int player_num = 0;
        if (player_slots_[0] == 0) {
            player_slots_[0] = id;
            player_num = 1;
        } else if (player_slots_[1] == 0) {
            player_slots_[1] = id;
            player_num = 2;
        } else {
            std::cout << "Rejected extra client " << id << "\n";
            return;
        }

        std::cout << "Player " << player_num << " joined (client " << id << ")\n";

        NetworkId paddle_id = entities_.spawn(PREFAB_PADDLE_BASE + static_cast<PrefabId>(player_num), {}, &net_);
        (void)paddle_id; // used for tracking
        paddle_net_ids_[player_num - 1] = paddle_id;
        rpc_.invoke_client(id, RPC_ASSIGN_PADDLE, paddle_id);

        if (player_num == 1) {
            ball_net_id_ = entities_.spawn(PREFAB_BALL, {}, &net_);
            score_net_id_ = entities_.spawn(PREFAB_SCORE, {}, &net_);
            (void)ball_net_id_; // used for game logic
        }

        // Late-joiner catch-up: send all existing entities to the new client.
        entities_.send_full_state_to_client(id, net_);
    }

    void on_client_disconnected(ClientId id) {
        if (player_slots_[0] == id)
            player_slots_[0] = 0;
        if (player_slots_[1] == id)
            player_slots_[1] = 0;
        std::cout << "Client " << id << " disconnected\n";
    }

    void game_tick() {
        for (int i = 0; i < 2; ++i) {
            if (player_slots_[i] == 0)
                continue;
            auto it = player_inputs_.find(player_slots_[i]);
            float dy = (it != player_inputs_.end()) ? static_cast<float>(it->second) * PADDLE_SPEED * DT
                                                     : 0.0f;
            NetworkId pid = paddle_net_ids_[i];
            auto pit = bridge_.paddle_y.find(pid);
            if (pit != bridge_.paddle_y.end()) {
                pit->second += dy;
                pit->second = std::clamp(pit->second, PADDLE_H / 2.0f + 1.0f,
                                         WORLD_H - PADDLE_H / 2.0f - 1.0f);
                repl_.mark_dirty(pid);
            }
        }

        bool both = (player_slots_[0] != 0 && player_slots_[1] != 0);
        if (!both || ball_net_id_ == 0)
            return;

        BallState& b = bridge_.ball;
        b.x += b.vx * DT;
        b.y += b.vy * DT;

        if (b.y <= 1.0f) {
            b.y = 1.0f;
            b.vy = std::abs(b.vy);
        }
        if (b.y >= WORLD_H - 2.0f) {
            b.y = WORLD_H - 2.0f;
            b.vy = -std::abs(b.vy);
        }

        for (int i = 0; i < 2; ++i) {
            NetworkId pid = paddle_net_ids_[i];
            auto pit = bridge_.paddle_y.find(pid);
            if (pit == bridge_.paddle_y.end())
                continue;
            float px = (i == 0) ? 2.0f : WORLD_W - 3.0f;
            float py = pit->second;
            if (std::abs(b.x - px) < 1.5f && std::abs(b.y - py) < PADDLE_H / 2.0f + 0.5f) {
                b.vx = (i == 0) ? std::abs(b.vx) : -std::abs(b.vx);
                float hit = (b.y - py) / (PADDLE_H / 2.0f);
                b.vy += hit * 6.0f;
                b.vy = std::clamp(b.vy, -BALL_SPEED * 1.5f, BALL_SPEED * 1.5f);
            }
        }

        if (b.x < 0.0f) {
            bridge_.score.p2++;
            reset_ball();
        } else if (b.x > WORLD_W) {
            bridge_.score.p1++;
            reset_ball();
        }

        repl_.mark_dirty(ball_net_id_);
        repl_.mark_dirty(score_net_id_);
    }

    void reset_ball() {
        bridge_.ball.x = WORLD_W / 2.0f;
        bridge_.ball.y = WORLD_H / 2.0f;
        bridge_.ball.vx = ((std::rand() % 2 == 0) ? 1.0f : -1.0f) * BALL_SPEED;
        bridge_.ball.vy = ((static_cast<float>(std::rand() % 100) / 100.0f) - 0.5f) * BALL_SPEED * 1.2f;
    }

    NetworkManager net_;
    RpcManager rpc_;
    PongBridge bridge_; // Must outlive entities_ and repl_
    NetworkEntityManager entities_;
    NetworkReplicationManager repl_;

    ClientId player_slots_[2] = {0, 0};
    std::unordered_map<ClientId, std::int8_t> player_inputs_;
    NetworkId paddle_net_ids_[2] = {0, 0};
    NetworkId ball_net_id_ = 0;
    NetworkId score_net_id_ = 0;
    float accumulator_ = 0.0f;
};

// ── Client ───────────────────────────────────────────────────────────────────

class PongClient {
public:
    PongClient(const std::string& host, std::uint16_t port, std::string name)
        : name_(std::move(name)) {
        rpc_.set_network_manager(&net_);
        net_.set_rpc_manager(&rpc_);

        rpc_.register_handler(RPC_ASSIGN_PADDLE, [this](const RpcParams& /*params*/, BitStream& args) {
            NetworkId id = 0;
            deserialize(args, id);
            my_paddle_id_ = id;
        });

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

        std::cout << "Connecting to " << host << ":" << port << "...\n";
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
        std::cout << "Connected as " << name_ << "!  W=up  S=down  Q=quit\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    ~PongClient() {
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
            predict_local_paddle(frame_dt);
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
                input_dy_ = -1;
                break;
            case 's':
            case 'S':
                input_dy_ = 1;
                break;
            case 'q':
            case 'Q':
                g_running = false;
                return;
            }
        }

        if (input_dy_ != 0) {
            input_timeout_ -= frame_dt_accum_;
            if (input_timeout_ <= 0.0f)
                input_dy_ = 0;
        }
        frame_dt_accum_ = 0.0f;
    }

    void predict_local_paddle(float dt) {
        frame_dt_accum_ += dt;
        input_timeout_ = 0.15f;

        if (my_paddle_id_ == 0)
            return;

        predicted_y_ += static_cast<float>(input_dy_) * PADDLE_SPEED * dt;
        predicted_y_ = std::clamp(predicted_y_, PADDLE_H / 2.0f + 1.0f,
                                  WORLD_H - PADDLE_H / 2.0f - 1.0f);

        // Gentle reconciliation toward server-authoritative position
        auto it = bridge_.paddle_y.find(my_paddle_id_);
        if (it != bridge_.paddle_y.end()) {
            float diff = it->second - predicted_y_;
            if (std::abs(diff) > 0.3f)
                predicted_y_ += diff * 0.25f;
        }

        input_accum_ += dt;
        if (input_accum_ >= DT) {
            input_accum_ -= DT;
            rpc_.invoke_server(RPC_PADDLE_INPUT, input_dy_);
        }
    }

    void render() {
        clear_screen();

        std::cout << "  P1: " << static_cast<int>(bridge_.score.p1)
                  << "     P2: " << static_cast<int>(bridge_.score.p2)
                  << "     You: " << name_ << "\n";

        std::cout << "+";
        for (int x = 0; x < static_cast<int>(WORLD_W); ++x)
            std::cout << "-";
        std::cout << "+  [paddles=" << bridge_.paddle_y.size() << " ball=" << bridge_.has_ball
                  << " my=" << my_paddle_id_ << "]\n";

        int gw = static_cast<int>(WORLD_W);
        int gh = static_cast<int>(WORLD_H);
        std::vector<std::string> grid(static_cast<std::size_t>(gh),
                                      std::string(static_cast<std::size_t>(gw), ' '));

        if (bridge_.has_ball) {
            int bx = static_cast<int>(std::round(bridge_.ball.x));
            int by = static_cast<int>(std::round(bridge_.ball.y));
            if (bx >= 0 && bx < gw && by >= 0 && by < gh)
                grid[static_cast<std::size_t>(by)][static_cast<std::size_t>(bx)] = 'o';
        }

        for (const auto& [pid, py_pos] : bridge_.paddle_y) {
            float y_render = py_pos;
            if (pid == my_paddle_id_)
                y_render = predicted_y_;

            bool is_left = true;
            auto prefab_it = bridge_.entity_prefab.find(pid);
            if (prefab_it != bridge_.entity_prefab.end() &&
                prefab_it->second == static_cast<PrefabId>(PREFAB_PADDLE_BASE + 2)) {
                is_left = false;
            }

            int px = is_left ? 1 : gw - 2;
            int py = static_cast<int>(std::round(y_render));
            int half = static_cast<int>(PADDLE_H) / 2;

            (void)prefab_it; // used for is_left calculation

            for (int ddy = -half; ddy <= half; ++ddy) {
                int gy = py + ddy;
                if (gy >= 0 && gy < gh && px >= 0 && px < gw)
                    grid[static_cast<std::size_t>(gy)][static_cast<std::size_t>(px)] =
                        (pid == my_paddle_id_) ? '#' : '|';
            }
        }

        int mid = gw / 2;
        for (int row = 0; row < gh; ++row) {
            if (grid[static_cast<std::size_t>(row)][static_cast<std::size_t>(mid)] == ' ')
                grid[static_cast<std::size_t>(row)][static_cast<std::size_t>(mid)] = ':';
        }

        for (int row = 0; row < gh; ++row)
            std::cout << "|" << grid[static_cast<std::size_t>(row)] << "|\n";

        std::cout << "+";
        for (int x = 0; x < gw; ++x)
            std::cout << "-";
        std::cout << "+  W=up S=down Q=quit\n";
    }

    NetworkManager net_;
    RpcManager rpc_;
    PongBridge bridge_; // Must outlive entities_ and repl_
    NetworkEntityManager entities_;
    NetworkReplicationManager repl_;
    std::string name_;

    NetworkId my_paddle_id_ = 0;
    float predicted_y_ = WORLD_H / 2.0f;
    std::int8_t input_dy_ = 0;
    float input_timeout_ = 0.0f;
    float frame_dt_accum_ = 0.0f;
    float input_accum_ = 0.0f;
};

// ── Entry point ──────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cout << "campello_net pong example " << version_string() << "\n\n";
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
        PongServer server(port);
        server.run();
    } else if (mode == "--client" || mode == "-c") {
        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }
        std::string host = argv[2];
        std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argv[3]));
        std::string name = argv[4];
        PongClient client(host, port, name);
        client.run();
    } else {
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}

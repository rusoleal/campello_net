#include <atomic>
#include <campello_net/network_manager.hpp>
#include <campello_net/rpc_manager.hpp>
#include <campello_net/serialization/bit_stream.hpp>
#include <campello_net/serialization/serializable.hpp>
#include <campello_net/transport/address.hpp>
#include <campello_net/version.hpp>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::serialization;

// ── RPC IDs ──────────────────────────────────────────────────────────────────

namespace rpc_id {
constexpr std::uint16_t chat_message = 1;
// constexpr std::uint16_t system_notice = 2; // reserved for future use
} // namespace rpc_id

// ── Global state for signal handling ─────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void on_sigint(int) {
    g_running = false;
}

// ── Non-blocking stdin (macOS / POSIX) ───────────────────────────────────────

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

/// Try to read a complete line from non-blocking stdin.
/// @return true if a full line was consumed.
static bool try_read_line(std::string& out_line) {
    static std::string buffer;
    char ch = 0;
    while (::read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == '\n') {
            out_line = buffer;
            buffer.clear();
            return true;
        }
        buffer.push_back(ch);
    }
    return false;
}

// ── Server ───────────────────────────────────────────────────────────────────

class ChatServer {
public:
    explicit ChatServer(std::uint16_t port) {
        net_.on_client_connected([](ClientId id) {
            std::cout << "[Server] Client " << id << " connected.\n";
        });
        net_.on_client_disconnected([](ClientId id) {
            std::cout << "[Server] Client " << id << " disconnected.\n";
        });

        rpc_.set_network_manager(&net_);
        rpc_.register_handler(rpc_id::chat_message, [this](const RpcParams& /*params*/, BitStream& args) {
            std::string username;
            std::string message;
            if (!deserialize(args, username) || !deserialize(args, message))
                return;

            std::cout << "[Server] " << username << ": " << message << "\n";

            // Broadcast to all clients (including sender so they see their own message echoed)
            rpc_.invoke_broadcast(rpc_id::chat_message, username, message);
        });
        net_.set_rpc_manager(&rpc_);

        transport::Address bind_addr("::", port); // dual-stack IPv6 (accepts IPv4-mapped too)
        if (!net_.start(NetworkManager::Config{
                NetworkManager::Mode::Server, bind_addr, {}, 32, 10.0f, 8192, 0.0f, 0.0f, 0.0f, 0.0f})) {
            std::cerr << "Failed to start server on port " << port << "\n";
            std::exit(1);
        }

        std::cout << "campello_net chat server listening on port " << port << "\n";
        std::cout << "Press Ctrl+C to stop.\n";
    }

    void run() {
        while (g_running) {
            net_.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        net_.stop();
        std::cout << "[Server] Shut down.\n";
    }

private:
    NetworkManager net_;
    RpcManager rpc_;
};

// ── Client ───────────────────────────────────────────────────────────────────

class ChatClient {
public:
    ChatClient(const std::string& host, std::uint16_t port, std::string username) : username_(std::move(username)) {
        rpc_.set_network_manager(&net_);
        rpc_.register_handler(rpc_id::chat_message, [](const RpcParams& /*params*/, BitStream& args) {
            std::string user;
            std::string msg;
            if (!deserialize(args, user) || !deserialize(args, msg))
                return;
            std::cout << "[" << user << "]: " << msg << "\n";
            std::cout.flush();
        });
        net_.set_rpc_manager(&rpc_);

        transport::Address server_addr(host, port);
        if (!net_.start(NetworkManager::Config{NetworkManager::Mode::Client, {}, server_addr})) {
            std::cerr << "Failed to connect to " << host << ":" << port << "\n";
            std::exit(1);
        }

        std::cout << "Connecting to " << host << ":" << port << " as '" << username_ << "'...\n";

        // Wait for connection approval
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

        std::cout << "Connected! Type messages and press Enter. Type /quit to exit.\n\n";
    }

    void run() {
        set_stdin_nonblocking(true);

        std::string line;
        while (g_running) {
            net_.poll();

            if (try_read_line(line)) {
                if (line == "/quit") {
                    g_running = false;
                    break;
                }
                if (!line.empty()) {
                    rpc_.invoke_server(rpc_id::chat_message, username_, line);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        set_stdin_nonblocking(false);
        net_.stop();
        std::cout << "[Client] Disconnected.\n";
    }

private:
    NetworkManager net_;
    RpcManager rpc_;
    std::string username_;
};

// ── Entry point ──────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cout << "campello_net chat example " << version_string() << "\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << prog << " --server <port>\n";
    std::cout << "  " << prog << " --client <host> <port> <username>\n";
    std::cout << "\nExamples:\n";
    std::cout << "  " << prog << " --server 7777\n";
    std::cout << "  " << prog << " --client 127.0.0.1 7777 alice\n";
    std::cout << "  " << prog << " --client ::1 7777 bob\n";
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
        ChatServer server(port);
        server.run();
    } else if (mode == "--client" || mode == "-c") {
        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }
        std::string host = argv[2];
        std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argv[3]));
        std::string username = argv[4];
        ChatClient client(host, port, username);
        client.run();
    } else {
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}

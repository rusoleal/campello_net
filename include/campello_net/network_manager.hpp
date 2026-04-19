#pragma once

#include "campello_net/net_stats.hpp"
#include "campello_net/network_time.hpp"
#include "campello_net/transport/address.hpp"
#include "campello_net/transport/i_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace systems::leal::campello_net {

using ClientId = std::uint64_t;

/// Unified entry point for multiplayer networking.
///
/// Supports three modes:
///   - **Server**: listens for incoming connections, assigns ClientIds
///   - **Client**: connects to a remote server
///   - **Host**: local server + local client in one process (loopback-optimised)
///
/// Multi-transport support allows a single server to accept connections over
/// multiple protocols simultaneously (e.g. UDP + Bluetooth). Use
/// set_transport() / add_transport() before calling start().
class NetworkManager {
public:
    enum class Mode { None, Server, Client, Host };

    struct Config {
        Mode mode = Mode::None;
        transport::Address bind_address;           ///< Local address to bind (Server/Host)
        transport::Address server_address;         ///< Remote server to connect to (Client/Host)
        std::size_t max_clients = 32;              ///< Max incoming connections (Server/Host)
        float connection_timeout_sec = 10.0f;      ///< Transport-level timeout

        // ── Rate limiting & security ──
        std::size_t max_packet_size = 8192;        ///< Drop packets larger than this (bytes)
        float max_messages_per_sec = 100.0f;       ///< 0 = unlimited
        float max_bytes_per_sec = 1024.0f * 1024.0f; ///< 0 = unlimited
        float max_rpcs_per_sec = 50.0f;            ///< 0 = unlimited
        float rate_limit_burst = 10.0f;            ///< Token bucket burst size
    };

    // ── Lifecycle ────────────────────────────────────────────────────────────

    NetworkManager();
    ~NetworkManager();

    NetworkManager(const NetworkManager&) = delete;
    NetworkManager& operator=(const NetworkManager&) = delete;
    NetworkManager(NetworkManager&&) noexcept;
    NetworkManager& operator=(NetworkManager&&) noexcept;

    /// Replace the default primary transport. Must be called before start().
    void set_transport(std::unique_ptr<transport::ITransport> transport);

    /// Add an additional transport for multi-transport server scenarios.
    /// Must be called before start().
    void add_transport(std::unique_ptr<transport::ITransport> transport);

    bool start(const Config& config);
    void stop();

    /// Must be called regularly (e.g. every frame or fixed tick).
    void poll();

    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] bool is_active() const noexcept;

    // ── Local clients (couch players) ────────────────────────────────────────

    /// Add a local client that does not use any network transport.
    /// Valid in Server and Host modes. Returns 0 on failure.
    ClientId add_local_client();

    /// Remove a local client previously added with add_local_client().
    void remove_local_client(ClientId client);

    // ── Sending ──────────────────────────────────────────────────────────────

    /// Server/Host: send to a specific client.
    bool send(ClientId client, const std::uint8_t* data, std::size_t length,
              transport::PacketReliability reliability = transport::PacketReliability::ReliableOrdered);

    /// Server/Host: broadcast to all connected clients.
    void broadcast(const std::uint8_t* data, std::size_t length,
                   transport::PacketReliability reliability = transport::PacketReliability::ReliableOrdered,
                   ClientId exclude = 0);

    /// Client: send to the server.
    bool send(const std::uint8_t* data, std::size_t length,
              transport::PacketReliability reliability = transport::PacketReliability::ReliableOrdered);

    // ── Receiving ────────────────────────────────────────────────────────────

    struct ReceivedMessage {
        ClientId client = 0;               ///< Sender (or 0 for server-to-client messages)
        std::vector<std::uint8_t> payload;
    };

    /// Pop the next user message. Returns false when queue is empty.
    bool pop_message(ReceivedMessage& out_msg);

    // ── Callbacks ────────────────────────────────────────────────────────────

    using ClientCallback = std::function<void(ClientId)>;
    using DataCallback = std::function<void(ClientId, const std::uint8_t*, std::size_t)>;

    void on_client_connected(ClientCallback cb);
    void on_client_disconnected(ClientCallback cb);
    void on_data_received(DataCallback cb);

    // ── Connection approval (Server / Host only) ─────────────────────────────

    /// Callback invoked when a transport-level connection is established.
    /// Return true to accept, false to reject. `payload` contains client-supplied data (auth token, version, …).
    using ApprovalCallback = std::function<bool(const transport::Address&, const std::vector<std::uint8_t>&)>;
    void set_connection_approval(ApprovalCallback cb);

    /// Server / Host: disconnect a specific client.
    void disconnect_client(ClientId client);

    /// Client: disconnect from server.
    void disconnect();

    // ── Queries ──────────────────────────────────────────────────────────────

    [[nodiscard]] std::size_t client_count() const noexcept;
    [[nodiscard]] bool is_client_connected(ClientId client) const noexcept;
    [[nodiscard]] transport::Address client_address(ClientId client) const;

    [[nodiscard]] float client_rtt(ClientId client) const noexcept;
    [[nodiscard]] float client_packet_loss(ClientId client) const noexcept;

    /// Per-client network statistics (Server / Host only).
    [[nodiscard]] NetStats net_stats(ClientId client) const noexcept;

    /// Local clock synchronized to server time (Client mode only).
    [[nodiscard]] double network_time() const noexcept;

    [[nodiscard]] ClientId local_client_id() const noexcept;

    // ── Entity integration (Phase 5) ─────────────────────────────────────────

    void set_entity_manager(class NetworkEntityManager* mgr) noexcept;

    // ── Replication integration (Phase 6) ────────────────────────────────────

    void set_replication_manager(class NetworkReplicationManager* mgr) noexcept;

    // ── RPC integration (Phase 9) ────────────────────────────────────────────

    void set_rpc_manager(class RpcManager* mgr) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace systems::leal::campello_net

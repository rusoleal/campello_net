#pragma once

#include "campello_net/network_entity.hpp"
#include "campello_net/network_manager.hpp"
#include "campello_net/rpc_params.hpp"
#include "campello_net/serialization/bit_stream.hpp"
#include "campello_net/serialization/serializable.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <utility>

namespace systems::leal::campello_net {

/// Lightweight RPC dispatcher.
///
/// Register handler functions on the receiving peer and invoke them from the
/// sending peer. Arguments are automatically serialized via the existing
/// `serialization::serialize` / `serialization::deserialize` free functions.
///
/// Wire format (system message type 0x22):
///   [rpc_id: std::uint16_t]
///   [args: BitStream blob]
///
/// Usage example:
///   // Server registers a handler for client→server RPC
///   rpc_mgr.register_handler(1, [](const RpcParams& params, BitStream& args) {
///       int damage = 0;
///       deserialize(args, damage);
///       apply_damage(params.sender, damage);
///   });
///
///   // Client calls the RPC
///   rpc_mgr.invoke_server(1, 42);   // template forwards 42 as argument
class RpcManager {
public:
    using Handler = std::function<void(const RpcParams& params, serialization::BitStream& args)>;

    RpcManager() = default;
    ~RpcManager() = default;

    RpcManager(const RpcManager&) = delete;
    RpcManager& operator=(const RpcManager&) = delete;

    // ── Configuration ────────────────────────────────────────────────────────

    /// Wire the NetworkManager for send/receive and mode queries.
    void set_network_manager(NetworkManager* net) noexcept;

    /// Wire the EntityManager for owner-aware RPC routing (invoke_owner / invoke_not_owner).
    void set_entity_manager(NetworkEntityManager* mgr) noexcept;

    /// Maximum RPC argument payload in bytes (0 = unlimited).
    /// Oversized outgoing RPCs are silently dropped; oversized incoming RPCs are rejected.
    void set_max_payload_size(std::size_t max) noexcept;

    /// Register a handler for incoming RPCs with id @p rpc_id.
    /// Overwrites any previous handler with the same id.
    /// @param authority Who is allowed to invoke this RPC (default: Anyone).
    void register_handler(std::uint16_t rpc_id, Handler handler,
                          RpcAuthority authority = RpcAuthority::Anyone);

    /// Remove a previously registered handler. No-op if @p rpc_id was not registered.
    void unregister_handler(std::uint16_t rpc_id);

    /// Set a per-RPC type rate limit (global across all senders). 0 = unlimited.
    void set_rpc_rate_limit(std::uint16_t rpc_id, float max_per_sec, float burst);

    // ── Invocation (variadic helpers) ────────────────────────────────────────

    /// Server → Client: invoke RPC @p rpc_id on @p client.
    template <typename... Args> void invoke_client(ClientId client, std::uint16_t rpc_id, Args&&... args);

    /// Client → Server: invoke RPC @p rpc_id on the server.
    template <typename... Args> void invoke_server(std::uint16_t rpc_id, Args&&... args);

    /// Server → All Clients: invoke RPC @p rpc_id on every connected client.
    template <typename... Args> void invoke_broadcast(std::uint16_t rpc_id, Args&&... args);

    /// Server → Owner: invoke RPC @p rpc_id on the client that owns @p entity_id.
    template <typename... Args> void invoke_owner(std::uint16_t rpc_id, NetworkId entity_id, Args&&... args);

    /// Server → All except Owner: invoke RPC @p rpc_id on every client except the owner of @p entity_id.
    template <typename... Args> void invoke_not_owner(std::uint16_t rpc_id, NetworkId entity_id, Args&&... args);

    // ── Low-level invocation (BitStream args) ────────────────────────────────

    /// Server → Client.
    void invoke_client(ClientId client, std::uint16_t rpc_id, const serialization::BitStream& args);

    /// Client → Server.
    void invoke_server(std::uint16_t rpc_id, const serialization::BitStream& args);

    /// Server → All Clients.
    void invoke_broadcast(std::uint16_t rpc_id, const serialization::BitStream& args);

    /// Server → Owner.
    void invoke_owner(std::uint16_t rpc_id, NetworkId entity_id, const serialization::BitStream& args);

    /// Server → All except Owner.
    void invoke_not_owner(std::uint16_t rpc_id, NetworkId entity_id, const serialization::BitStream& args);

    // ── Receiving ────────────────────────────────────────────────────────────

    /// Called by NetworkManager when an RPC system message arrives.
    void on_receive(ClientId sender, const std::uint8_t* data, std::size_t len);

private:
    struct HandlerEntry {
        Handler handler;
        RpcAuthority authority = RpcAuthority::Anyone;
    };

    struct TokenBucket {
        float rate_per_sec = 0.0f;
        float burst = 0.0f;
        float tokens = 0.0f;
        double last_update = 0.0;
    };

    NetworkManager* net_ = nullptr;
    NetworkEntityManager* entity_mgr_ = nullptr;
    std::size_t max_payload_size_ = 0;
    std::unordered_map<std::uint16_t, HandlerEntry> handlers_;
    std::unordered_map<std::uint16_t, TokenBucket> rpc_rate_buckets_;

    [[nodiscard]] std::vector<std::uint8_t> build_rpc_packet(std::uint16_t rpc_id,
                                                              const serialization::BitStream& args) const;
    void send_rpc(ClientId target_client, std::uint16_t rpc_id, const serialization::BitStream& args);
};

// ── Template implementations ────────────────────────────────────────────────

template <typename... Args> void RpcManager::invoke_client(ClientId client, std::uint16_t rpc_id, Args&&... args) {
    serialization::BitStream stream;
    (serialization::serialize(stream, std::forward<Args>(args)), ...);
    send_rpc(client, rpc_id, stream);
}

template <typename... Args> void RpcManager::invoke_server(std::uint16_t rpc_id, Args&&... args) {
    serialization::BitStream stream;
    (serialization::serialize(stream, std::forward<Args>(args)), ...);
    send_rpc(0, rpc_id, stream);
}

template <typename... Args> void RpcManager::invoke_broadcast(std::uint16_t rpc_id, Args&&... args) {
    serialization::BitStream stream;
    (serialization::serialize(stream, std::forward<Args>(args)), ...);
    if (!net_)
        return;
    auto packet = build_rpc_packet(rpc_id, stream);
    if (packet.empty())
        return; // oversized
    net_->broadcast(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
}

template <typename... Args> void RpcManager::invoke_owner(std::uint16_t rpc_id, NetworkId entity_id, Args&&... args) {
    serialization::BitStream stream;
    (serialization::serialize(stream, std::forward<Args>(args)), ...);
    if (!entity_mgr_)
        return;
    ClientId owner = entity_mgr_->owner(entity_id);
    if (owner == 0)
        return;
    send_rpc(owner, rpc_id, stream);
}

template <typename... Args> void RpcManager::invoke_not_owner(std::uint16_t rpc_id, NetworkId entity_id, Args&&... args) {
    serialization::BitStream stream;
    (serialization::serialize(stream, std::forward<Args>(args)), ...);
    if (!net_ || !entity_mgr_)
        return;
    ClientId owner = entity_mgr_->owner(entity_id);
    auto packet = build_rpc_packet(rpc_id, stream);
    if (packet.empty())
        return; // oversized
    net_->broadcast(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered, owner);
}

} // namespace systems::leal::campello_net

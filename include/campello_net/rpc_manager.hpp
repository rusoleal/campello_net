#pragma once

#include "campello_net/network_manager.hpp"
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
///   [rpc_id: uint16_t]
///   [args: BitStream blob]
///
/// Usage example:
///   // Server registers a handler for client→server RPC
///   rpc_mgr.register_handler(1, [](ClientId sender, BitStream& args) {
///       int damage = 0;
///       deserialize(args, damage);
///       apply_damage(sender, damage);
///   });
///
///   // Client calls the RPC
///   rpc_mgr.invoke_server(1, 42);   // template forwards 42 as argument
class RpcManager {
public:
    using Handler = std::function<void(ClientId sender, serialization::BitStream& args)>;

    RpcManager() = default;
    ~RpcManager() = default;

    RpcManager(const RpcManager&) = delete;
    RpcManager& operator=(const RpcManager&) = delete;

    // ── Configuration ────────────────────────────────────────────────────────

    void set_network_manager(NetworkManager* net) noexcept;

    /// Register a handler for incoming RPCs with id @p rpc_id.
    /// Overwrites any previous handler with the same id.
    void register_handler(std::uint16_t rpc_id, Handler handler);

    /// Remove a previously registered handler.
    void unregister_handler(std::uint16_t rpc_id);

    // ── Invocation (variadic helpers) ────────────────────────────────────────

    /// Server → Client: invoke RPC @p rpc_id on @p client.
    template <typename... Args> void invoke_client(ClientId client, std::uint16_t rpc_id, Args&&... args);

    /// Client → Server: invoke RPC @p rpc_id on the server.
    template <typename... Args> void invoke_server(std::uint16_t rpc_id, Args&&... args);

    // ── Low-level invocation (BitStream args) ────────────────────────────────

    /// Server → Client.
    void invoke_client(ClientId client, std::uint16_t rpc_id, const serialization::BitStream& args);

    /// Client → Server.
    void invoke_server(std::uint16_t rpc_id, const serialization::BitStream& args);

    // ── Receiving ────────────────────────────────────────────────────────────

    /// Called by NetworkManager when an RPC system message arrives.
    void on_receive(ClientId sender, const std::uint8_t* data, std::size_t len);

private:
    NetworkManager* net_ = nullptr;
    std::unordered_map<std::uint16_t, Handler> handlers_;

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

} // namespace systems::leal::campello_net

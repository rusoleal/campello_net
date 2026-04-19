#pragma once

#include "campello_net/transport/address.hpp"
#include "campello_net/transport/message.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace systems::leal::campello_net::transport {

using ClientId = std::uint64_t;

/// Information about a connected client.
struct ConnectionInfo {
    ClientId id = 0;
    Address address;
    float rtt = 0.0f;
    float packet_loss = 0.0f;
};

/// Abstract peer interface for client/server/host topologies.
class INetworkPeer {
public:
    virtual ~INetworkPeer() = default;

    /// Send a message to a specific client.
    virtual bool send(ClientId client, const Message& message) = 0;

    /// Broadcast a message to all connected clients.
    /// @param exclude Client to skip (0 = broadcast to everyone).
    virtual void broadcast(const Message& message, ClientId exclude = 0) = 0;

    /// Receive the next incoming message.
    /// @return true if a message was available.
    virtual bool receive(ClientId& out_client, Message& out_message) = 0;

    /// Number of currently connected clients.
    [[nodiscard]] virtual std::size_t client_count() const = 0;

    /// Get info for a specific client.
    [[nodiscard]] virtual ConnectionInfo get_client_info(ClientId client) const = 0;

    /// Disconnect a specific client.
    virtual void disconnect(ClientId client) = 0;

    /// Maximum number of clients this peer can accept.
    [[nodiscard]] virtual std::size_t max_clients() const = 0;

    /// Local address this peer is bound to.
    [[nodiscard]] virtual Address local_address() const = 0;
};

} // namespace systems::leal::campello_net::transport

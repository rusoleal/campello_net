#pragma once

#include "campello_net/transport/address.hpp"
#include "campello_net/transport/packet.hpp"

#include <cstddef>
#include <cstdint>

namespace systems::leal::campello_net::transport {

/// Abstract transport interface.
///
/// Implementations are non-blocking. Call poll() each frame to process
/// incoming data and update connection state.
class ITransport {
public:
    virtual ~ITransport() = default;

    /// Bind as a server on the given local address.
    virtual bool bind(const Address& address) = 0;

    /// Connect as a client to a remote server.
    virtual bool connect(const Address& address) = 0;

    /// Close the socket and reset all state.
    virtual void disconnect() = 0;

    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    /// Send data to the remote peer (client mode) or to all connected peers (server mode).
    virtual bool send(const std::uint8_t* data, std::size_t length, PacketReliability reliability) = 0;

    /// Send data to a specific connected peer (server mode).
    ///
    /// Transports that do not support targeted sends (e.g. pure datagram
    /// broadcast layers) may return false.
    virtual bool send_to(const Address& address, const std::uint8_t* data, std::size_t length,
                         PacketReliability reliability) {
        (void)address;
        (void)data;
        (void)length;
        (void)reliability;
        return false;
    }

    /// Read pending socket data and advance connection state.
    virtual void poll() = 0;

    /// Pop one received user payload. Returns false when queue is empty.
    virtual bool pop_receive(std::uint8_t* buffer, std::size_t max_length, std::size_t& out_length, Address& out_sender) = 0;

    /// Round-trip time in seconds (moving average).
    [[nodiscard]] virtual float rtt() const noexcept = 0;

    /// Estimated packet loss ratio [0,1].
    [[nodiscard]] virtual float packet_loss() const noexcept = 0;

    /// RTT for a specific connection (0 if not connected or unsupported).
    [[nodiscard]] virtual float get_connection_rtt(const Address& address) const noexcept {
        (void)address;
        return 0.0f;
    }

    /// Packet loss for a specific connection (0 if not connected or unsupported).
    [[nodiscard]] virtual float get_connection_packet_loss(const Address& address) const noexcept {
        (void)address;
        return 0.0f;
    }
};

} // namespace systems::leal::campello_net::transport

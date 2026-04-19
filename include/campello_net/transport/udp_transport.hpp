#pragma once

#include "campello_net/transport/i_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace systems::leal::campello_net::transport {

/// UDP-based transport with connection-oriented semantics.
///
/// Features:
/// - IPv4 / IPv6 dual-stack sockets
/// - Unreliable, reliable ordered, reliable unordered, unreliable sequenced channels
/// - Automatic packet fragmentation & reassembly
/// - RTT measurement, sliding window, ack piggybacking
/// - Connection handshake & keep-alive
class UdpTransport : public ITransport {
public:
    UdpTransport();
    ~UdpTransport() override;

    // Non-copyable, non-movable (holds socket fd)
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;
    UdpTransport(UdpTransport&&) = delete;
    UdpTransport& operator=(UdpTransport&&) = delete;

    bool bind(const Address& address) override;
    bool connect(const Address& address) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override;

    bool send(const uint8_t* data, std::size_t length, PacketReliability reliability) override;

    void poll() override;

    bool pop_receive(uint8_t* buffer, std::size_t max_length, std::size_t& out_length, Address& out_sender) override;

    [[nodiscard]] float rtt() const noexcept override;
    [[nodiscard]] float packet_loss() const noexcept override;

    /// Targeted send to a specific connected peer (server mode).
    bool send_to(const Address& address, const uint8_t* data, std::size_t length,
                 PacketReliability reliability) override;

    /// Send with priority (higher values sent first when bandwidth is constrained).
    bool send_with_priority(const Address& address, const uint8_t* data, std::size_t length,
                            PacketReliability reliability, uint8_t priority);

    /// Set per-connection bandwidth limit (bytes/sec, 0 = unlimited).
    void set_connection_bandwidth_limit(const Address& address, std::uint32_t bytes_per_second);

    /// Set per-channel bandwidth limit for a specific connection (bytes/sec, 0 = unlimited).
    void set_channel_bandwidth_limit(const Address& address, PacketReliability reliability,
                                     std::uint32_t bytes_per_second);

    /// Get RTT for a specific connection (0 if not connected).
    [[nodiscard]] float get_connection_rtt(const Address& address) const noexcept override;

    /// Get packet loss for a specific connection (0 if not connected).
    [[nodiscard]] float get_connection_packet_loss(const Address& address) const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace systems::leal::campello_net::transport

#pragma once

#include "campello_net/transport/i_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace systems::leal::campello_net::transport {

/// @brief UDP-based transport with connection-oriented semantics.
///
/// Features:
/// - IPv4 / IPv6 dual-stack sockets (AF_INET6 with IPV6_V6ONLY=0)
/// - Unreliable, reliable ordered, reliable unordered, unreliable sequenced channels
/// - Automatic packet fragmentation & reassembly
/// - RTT measurement, sliding window, ACK piggybacking
/// - Connection handshake & keep-alive
///
/// This is the default transport for NetworkManager.
class UdpTransport : public ITransport {
public:
    UdpTransport();
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;
    UdpTransport(UdpTransport&&) = delete;
    UdpTransport& operator=(UdpTransport&&) = delete;

    // ── ITransport implementation ───────────────────────────────────────────

    /// @brief Bind to a local address and start listening.
    bool bind(const Address& address) override;

    /// @brief Initiate a connection handshake to a remote server.
    bool connect(const Address& address) override;

    /// @brief Disconnect and close the socket.
    void disconnect() override;

    /// @return true if fully connected.
    [[nodiscard]] bool is_connected() const noexcept override;

    /// @brief Send data to the connected peer.
    bool send(const std::uint8_t* data, std::size_t length, PacketReliability reliability) override;

    /// @brief Poll for incoming packets and update internal timers.
    void poll() override;

    /// @brief Pop a received packet from the internal queue.
    bool pop_receive(std::uint8_t* buffer, std::size_t max_length, std::size_t& out_length,
                     Address& out_sender) override;

    /// @return Smoothed RTT of the default connection.
    [[nodiscard]] float rtt() const noexcept override;

    /// @return Estimated packet loss ratio [0, 1].
    [[nodiscard]] float packet_loss() const noexcept override;

    /// @brief Send to a specific connected peer (server mode).
    bool send_to(const Address& address, const std::uint8_t* data, std::size_t length,
                 PacketReliability reliability) override;

    // ── Extended API ────────────────────────────────────────────────────────

    /// @brief Send with explicit priority (higher values sent first under bandwidth pressure).
    bool send_with_priority(const Address& address, const std::uint8_t* data, std::size_t length,
                            PacketReliability reliability, std::uint8_t priority);

    /// @brief Set per-connection bandwidth limit.
    /// @param address Remote endpoint.
    /// @param bytes_per_second 0 = unlimited.
    void set_connection_bandwidth_limit(const Address& address, std::uint32_t bytes_per_second);

    /// @brief Set per-channel bandwidth limit for a specific connection.
    void set_channel_bandwidth_limit(const Address& address, PacketReliability reliability,
                                     std::uint32_t bytes_per_second);

    /// @return RTT for a specific connection (0 if not connected).
    [[nodiscard]] float get_connection_rtt(const Address& address) const noexcept override;

    /// @return Packet loss for a specific connection (0 if not connected).
    [[nodiscard]] float get_connection_packet_loss(const Address& address) const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace systems::leal::campello_net::transport

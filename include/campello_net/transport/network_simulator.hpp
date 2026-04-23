#pragma once

#include "campello_net/transport/i_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace systems::leal::campello_net::transport {

/// @brief Transport decorator that simulates adverse network conditions.
///
/// Wraps any ITransport and injects configurable latency, jitter, packet loss,
/// and duplication. Useful for testing multiplayer behavior under real-world
/// adverse conditions without leaving localhost.
///
/// Example:
/// @code
/// auto inner = std::make_unique<UdpTransport>();
/// auto sim = std::make_unique<NetworkSimulator>(std::move(inner));
/// sim->set_packet_loss(0.05f);
/// sim->set_latency(100.0f, 150.0f);
/// sim->set_jitter(20.0f);
/// @endcode
class NetworkSimulator : public ITransport {
public:
    explicit NetworkSimulator(std::unique_ptr<ITransport> inner);
    ~NetworkSimulator() override;

    NetworkSimulator(const NetworkSimulator&) = delete;
    NetworkSimulator& operator=(const NetworkSimulator&) = delete;
    NetworkSimulator(NetworkSimulator&&) noexcept;
    NetworkSimulator& operator=(NetworkSimulator&&) noexcept;

    // ── ITransport implementation ───────────────────────────────────────────

    bool bind(const Address& address) override;
    bool connect(const Address& address) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override;

    bool send(const std::uint8_t* data, std::size_t length, PacketReliability reliability) override;
    bool send_to(const Address& address, const std::uint8_t* data, std::size_t length,
                 PacketReliability reliability) override;
    void poll() override;

    bool pop_receive(std::uint8_t* buffer, std::size_t max_length, std::size_t& out_length,
                     Address& out_sender) override;

    [[nodiscard]] float rtt() const noexcept override;
    [[nodiscard]] float packet_loss() const noexcept override;
    [[nodiscard]] float get_connection_rtt(const Address& address) const noexcept override;
    [[nodiscard]] float get_connection_packet_loss(const Address& address) const noexcept override;

    // ── Simulation parameters ───────────────────────────────────────────────

    /// @brief Set the probability that an outbound packet is dropped.
    /// @param ratio Probability in range [0, 1].
    void set_packet_loss(float ratio);

    /// @brief Set fixed latency range.
    /// @param min_ms Minimum delay in milliseconds.
    /// @param max_ms Maximum delay in milliseconds.
    void set_latency(float min_ms, float max_ms);

    /// @brief Set additional random jitter on top of fixed latency.
    /// @param ms Standard deviation of jitter in milliseconds.
    void set_jitter(float ms);

    /// @brief Set the probability that a packet is duplicated.
    /// @param ratio Probability in range [0, 1].
    void set_duplication(float ratio);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace systems::leal::campello_net::transport

#pragma once

#include "campello_net/transport/i_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace systems::leal::campello_net::transport {

/// Transport decorator that simulates adverse network conditions.
/// Useful for testing multiplayer behaviour under packet loss, latency, and jitter.
class NetworkSimulator : public ITransport {
public:
    explicit NetworkSimulator(std::unique_ptr<ITransport> inner);
    ~NetworkSimulator() override;

    NetworkSimulator(const NetworkSimulator&) = delete;
    NetworkSimulator& operator=(const NetworkSimulator&) = delete;
    NetworkSimulator(NetworkSimulator&&) noexcept;
    NetworkSimulator& operator=(NetworkSimulator&&) noexcept;

    bool bind(const Address& address) override;
    bool connect(const Address& address) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override;

    bool send(const uint8_t* data, std::size_t length, PacketReliability reliability) override;
    bool send_to(const Address& address, const uint8_t* data, std::size_t length,
                 PacketReliability reliability) override;
    void poll() override;

    bool pop_receive(uint8_t* buffer, std::size_t max_length, std::size_t& out_length, Address& out_sender) override;

    [[nodiscard]] float rtt() const noexcept override;
    [[nodiscard]] float packet_loss() const noexcept override;
    [[nodiscard]] float get_connection_rtt(const Address& address) const noexcept override;
    [[nodiscard]] float get_connection_packet_loss(const Address& address) const noexcept override;

    // ── Simulation parameters ───────────────────────────────────────────────

    /// Probability [0,1] that an outbound packet is dropped.
    void set_packet_loss(float ratio);
    /// Fixed latency range in milliseconds.
    void set_latency(float min_ms, float max_ms);
    /// Additional random jitter in milliseconds.
    void set_jitter(float ms);
    /// Probability [0,1] that a packet is duplicated.
    void set_duplication(float ratio);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace systems::leal::campello_net::transport

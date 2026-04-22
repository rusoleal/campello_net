#pragma once

#include "campello_net/transport/i_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace systems::leal::campello_net::transport {

class LoopbackTransport;

/// Shared message router for `LoopbackTransport` instances.
/// Two transports can communicate only if they share the same hub.
class LoopbackHub {
public:
    LoopbackHub();
    ~LoopbackHub();

    LoopbackHub(const LoopbackHub&) = delete;
    LoopbackHub& operator=(const LoopbackHub&) = delete;

    void bind_server(const Address& addr, LoopbackTransport* t);
    void unbind_server(const Address& addr);
    bool connect_client(const Address& server_addr, LoopbackTransport* client, Address& out_client_addr);
    void disconnect_client(const Address& client_addr);
    void deliver(const Address& to, const Address& from, const std::uint8_t* data, std::size_t len,
                 PacketReliability reliability);
    void broadcast(const Address& server_addr, const Address& from, const std::uint8_t* data, std::size_t len,
                   PacketReliability reliability);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class LoopbackTransport;
};

/// In-memory transport for testing and local multiplayer.
///
/// All data is copied directly between transport instances via a shared
/// `LoopbackHub`. No sockets, no threads, no kernel calls. Useful for:
///   - Fast unit tests (no `sleep_for` polling loops)
///   - Single-process local multiplayer
///   - Reference implementation for future non-UDP transports
///
/// Two `LoopbackTransport` instances can communicate only if they share the
/// same `LoopbackHub`:
///
/// ```cpp
/// auto hub = std::make_shared<LoopbackHub>();
/// LoopbackTransport server(hub);
/// server.bind(Address("127.0.0.1", 1234));
///
/// LoopbackTransport client(hub);
/// client.connect(Address("127.0.0.1", 1234));
/// ```
class LoopbackTransport : public ITransport {
public:
    /// Create with an optional shared hub. Transports sharing a hub can
    /// communicate; transports with different hubs are isolated.
    explicit LoopbackTransport(std::shared_ptr<LoopbackHub> hub = nullptr);
    ~LoopbackTransport() override;

    LoopbackTransport(const LoopbackTransport&) = delete;
    LoopbackTransport& operator=(const LoopbackTransport&) = delete;
    LoopbackTransport(LoopbackTransport&&) noexcept;
    LoopbackTransport& operator=(LoopbackTransport&&) noexcept;

    bool bind(const Address& address) override;
    bool connect(const Address& address) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override;

    bool send(const std::uint8_t* data, std::size_t length, PacketReliability reliability) override;
    bool send_to(const Address& address, const std::uint8_t* data, std::size_t length,
                 PacketReliability reliability) override;

    void poll() override;

    bool pop_receive(std::uint8_t* buffer, std::size_t max_length, std::size_t& out_length, Address& out_sender) override;

    [[nodiscard]] float rtt() const noexcept override;
    [[nodiscard]] float packet_loss() const noexcept override;
    [[nodiscard]] float get_connection_rtt(const Address& address) const noexcept override;
    [[nodiscard]] float get_connection_packet_loss(const Address& address) const noexcept override;

    /// Configure artificial per-hop latency in seconds (default 0).
    void set_latency(float seconds) noexcept;

    /// Configure artificial packet loss ratio [0,1] (default 0).
    void set_packet_loss(float ratio) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class LoopbackHub;
    void enqueue_pending(const Address& sender, const std::uint8_t* data, std::size_t len, PacketReliability reliability);
};

} // namespace systems::leal::campello_net::transport

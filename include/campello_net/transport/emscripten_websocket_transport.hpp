#pragma once

#include "campello_net/transport/i_transport.hpp"

#ifdef CAMPELLO_NET_PLATFORM_WASM

#include <cstddef>
#include <cstdint>
#include <memory>

namespace systems::leal::campello_net::transport {

/// Browser WebSocket transport for Emscripten/WASM targets.
///
/// This transport uses the browser's native WebSocket API (via EM_JS) to
/// provide reliable-ordered messaging. It is client-only: bind() always
/// returns false because a browser cannot listen for raw socket connections.
///
/// PacketReliability is ignored — all WebSocket messages are reliable and
/// ordered by the underlying TCP transport.
class EmscriptenWebSocketTransport : public ITransport {
public:
    EmscriptenWebSocketTransport();
    ~EmscriptenWebSocketTransport() override;

    EmscriptenWebSocketTransport(const EmscriptenWebSocketTransport&) = delete;
    EmscriptenWebSocketTransport& operator=(const EmscriptenWebSocketTransport&) = delete;
    EmscriptenWebSocketTransport(EmscriptenWebSocketTransport&&) noexcept;
    EmscriptenWebSocketTransport& operator=(EmscriptenWebSocketTransport&&) noexcept;

    bool bind(const Address& address) override;
    bool connect(const Address& address) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const noexcept override;

    bool send(const std::uint8_t* data, std::size_t length, PacketReliability reliability) override;
    void poll() override;
    bool pop_receive(std::uint8_t* buffer, std::size_t max_length, std::size_t& out_length,
                     Address& out_sender) override;

    [[nodiscard]] float rtt() const noexcept override;
    [[nodiscard]] float packet_loss() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace systems::leal::campello_net::transport

#endif // CAMPELLO_NET_PLATFORM_WASM

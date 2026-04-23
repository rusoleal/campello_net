#pragma once

#include "campello_net/transport/i_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace systems::leal::campello_net::transport {

/// @brief Transport decorator that adds ChaCha20-Poly1305 encryption.
///
/// Wraps any ITransport and encrypts/decrypts NetworkManager-level payloads.
/// Transport-level control packets (handshake, ACKs, fragments) remain in
/// plaintext — only system messages, RPCs, replication data, and user payloads
/// are encrypted.
///
/// Usage:
/// @code
/// std::array<std::uint8_t, 32> key = derive_key_from_auth_token(...);
/// auto enc = std::make_unique<EncryptedTransport>(std::make_unique<UdpTransport>(), key);
/// nm.set_transport(std::move(enc));
/// @endcode
class EncryptedTransport : public ITransport {
public:
    static constexpr std::size_t KEY_SIZE = 32;

    /// @brief Wrap an inner transport and enable encryption with a pre-shared key.
    /// @param inner The underlying transport (e.g., UdpTransport).
    /// @param key 32-byte ChaCha20-Poly1305 key.
    explicit EncryptedTransport(std::unique_ptr<ITransport> inner, std::span<const std::uint8_t> key);
    ~EncryptedTransport() override;

    EncryptedTransport(const EncryptedTransport&) = delete;
    EncryptedTransport& operator=(const EncryptedTransport&) = delete;
    EncryptedTransport(EncryptedTransport&&) noexcept;
    EncryptedTransport& operator=(EncryptedTransport&&) noexcept;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace systems::leal::campello_net::transport

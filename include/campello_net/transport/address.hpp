#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace systems::leal::campello_net::transport {

/// Network endpoint address (IPv4 or IPv6).
class Address {
public:
    Address() noexcept = default;

    /// Bind-any address on the given port.
    explicit Address(uint16_t port) noexcept;

    /// Parse an IP string and port.
    Address(const std::string& ip, uint16_t port);

    [[nodiscard]] bool is_valid() const noexcept { return valid_; }
    [[nodiscard]] uint16_t port() const noexcept;
    [[nodiscard]] std::string ip() const;
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const Address& other) const noexcept;
    [[nodiscard]] bool operator!=(const Address& other) const noexcept;
    [[nodiscard]] bool operator<(const Address& other) const noexcept;

    // Internal accessors for transport implementations.
    [[nodiscard]] const std::byte* raw_storage() const noexcept { return storage_.data(); }
    [[nodiscard]] std::byte* raw_storage() noexcept { return storage_.data(); }
    [[nodiscard]] uint8_t raw_storage_size() const noexcept { return storage_len_; }
    void set_raw_storage(const std::byte* data, uint8_t len) noexcept;

private:
    friend class UdpTransport;

    alignas(16) std::array<std::byte, 128> storage_{};
    uint8_t storage_len_ = 0;
    bool valid_ = false;
};

} // namespace systems::leal::campello_net::transport

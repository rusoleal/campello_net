#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace systems::leal::campello_net::transport {

/// @brief Network endpoint address (IPv4 or IPv6).
///
/// Stores the raw socket address internally (sockaddr_in or sockaddr_in6)
/// and provides a uniform interface for IP string representation and comparison.
class Address {
public:
    /// @brief Create an invalid / uninitialized address.
    Address() noexcept = default;

    /// @brief Create a bind-any address (:: or 0.0.0.0) on the given port.
    /// @param port UDP port number.
    explicit Address(std::uint16_t port) noexcept;

    /// @brief Parse an IP string and port.
    /// @param ip IPv4 or IPv6 string (e.g., "127.0.0.1" or "::1").
    /// @param port UDP port number.
    Address(const std::string& ip, std::uint16_t port);

    /// @return true if the address was successfully parsed or constructed.
    [[nodiscard]] bool is_valid() const noexcept {
        return valid_;
    }

    /// @return The UDP port number in host byte order.
    [[nodiscard]] std::uint16_t port() const noexcept;

    /// @return The IP address as a human-readable string.
    [[nodiscard]] std::string ip() const;

    /// @return "ip:port" formatted string.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const Address& other) const noexcept;
    [[nodiscard]] bool operator!=(const Address& other) const noexcept;
    [[nodiscard]] bool operator<(const Address& other) const noexcept;

    /// @name Internal accessors for transport implementations
    /// @{

    /// @return Pointer to the raw sockaddr storage.
    [[nodiscard]] const std::byte* raw_storage() const noexcept {
        return storage_.data();
    }
    [[nodiscard]] std::byte* raw_storage() noexcept {
        return storage_.data();
    }
    /// @return Size of the valid portion of raw_storage (e.g., sizeof(sockaddr_in)).
    [[nodiscard]] std::uint8_t raw_storage_size() const noexcept {
        return storage_len_;
    }
    /// @brief Set raw storage directly from a sockaddr pointer.
    void set_raw_storage(const std::byte* data, std::uint8_t len) noexcept;

    /// @}

private:
    friend class UdpTransport;

    alignas(16) std::array<std::byte, 128> storage_{};
    std::uint8_t storage_len_ = 0;
    bool valid_ = false;
};

} // namespace systems::leal::campello_net::transport

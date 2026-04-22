#pragma once

#include <cstddef>
#include <cstdint>

namespace systems::leal::campello_net {

/// @brief Time-limited connection token for authenticated handshakes.
///
/// The server generates a token (signed with a secret key via HMAC-SHA256)
/// and gives it to the client out-of-band (e.g. via matchmaking, REST API,
/// or direct invite). The client presents the token inside its
/// `ConnectRequest` system message. The server validates the HMAC and
/// expiry before accepting the connection.
///
/// Token format (64 bytes):
///   Bytes 0..3   timestamp    uint32_t  LE  (Unix seconds when created)
///   Bytes 4..5   expiry_sec   uint16_t  LE  (validity window in seconds)
///   Bytes 6..13  client_id    uint64_t  LE  (reserved ClientId, 0 = auto)
///   Bytes 14..29 nonce        uint8_t[16]   (random, prevents replay)
///   Bytes 30..31 reserved     uint8_t[2]    (must be zero)
///   Bytes 32..63 hmac         uint8_t[32]   (HMAC-SHA256 of bytes 0..31)
struct ConnectionToken {
    static constexpr std::size_t SIZE = 64;

    /// @brief Generate a new token signed with `secret`.
    /// @param out_token Buffer of at least SIZE bytes.
    /// @param secret 32-byte HMAC key.
    /// @param expiry_seconds How many seconds the token remains valid.
    /// @param reserved_client_id Optional reserved ClientId (0 = auto-assign).
    /// @param current_time_unix Current Unix timestamp in seconds.
    /// @return true on success.
    static bool generate(std::uint8_t out_token[SIZE],
                         const std::uint8_t secret[32],
                         std::uint32_t expiry_seconds = 60,
                         std::uint64_t reserved_client_id = 0,
                         std::uint32_t current_time_unix = 0) noexcept;

    /// @brief Validate a token: verify HMAC, check expiry, check reserved bytes.
    /// @param token Token to validate.
    /// @param secret 32-byte HMAC key (must match generation key).
    /// @param current_time_unix Current Unix timestamp in seconds.
    /// @return true if token is valid and not expired.
    static bool validate(const std::uint8_t token[SIZE],
                         const std::uint8_t secret[32],
                         std::uint32_t current_time_unix) noexcept;

    /// @brief Extract the reserved ClientId from a token (0 if none).
    static std::uint64_t reserved_client_id(const std::uint8_t token[SIZE]) noexcept;
};

} // namespace systems::leal::campello_net

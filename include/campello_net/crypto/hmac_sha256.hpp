#pragma once

#include <cstddef>
#include <cstdint>

namespace systems::leal::campello_net::crypto {

/// @brief Compute HMAC-SHA256.
/// @param key HMAC key (any length).
/// @param key_len Key length in bytes.
/// @param message Message to authenticate.
/// @param message_len Message length in bytes.
/// @param out_mac Output buffer, must be at least 32 bytes.
void hmac_sha256(const std::uint8_t* key, std::size_t key_len,
                 const std::uint8_t* message, std::size_t message_len,
                 std::uint8_t out_mac[32]) noexcept;

/// @brief Compute raw SHA-256 digest.
/// @param data Input data.
/// @param len Input length.
/// @param out_digest Output buffer, must be at least 32 bytes.
void sha256(const std::uint8_t* data, std::size_t len,
            std::uint8_t out_digest[32]) noexcept;

} // namespace systems::leal::campello_net::crypto

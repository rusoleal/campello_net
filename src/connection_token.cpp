#include "campello_net/connection_token.hpp"

#include "campello_net/crypto/hmac_sha256.hpp"

#include <cstring>

namespace systems::leal::campello_net {

namespace {

inline void store32le(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline std::uint32_t load32le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void store64le(std::uint8_t* p, std::uint64_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
    p[4] = static_cast<std::uint8_t>(v >> 32);
    p[5] = static_cast<std::uint8_t>(v >> 40);
    p[6] = static_cast<std::uint8_t>(v >> 48);
    p[7] = static_cast<std::uint8_t>(v >> 56);
}

inline std::uint64_t load64le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(p[0]) |
           (static_cast<std::uint64_t>(p[1]) << 8) |
           (static_cast<std::uint64_t>(p[2]) << 16) |
           (static_cast<std::uint64_t>(p[3]) << 24) |
           (static_cast<std::uint64_t>(p[4]) << 32) |
           (static_cast<std::uint64_t>(p[5]) << 40) |
           (static_cast<std::uint64_t>(p[6]) << 48) |
           (static_cast<std::uint64_t>(p[7]) << 56);
}

inline void store16le(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

inline std::uint16_t load16le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

// Simple nonce filler: mixes time, expiry, and a counter to produce
// a deterministic-but-unique value for each token.  Not cryptographic
// randomness, but sufficient because the HMAC provides authenticity.
void fill_nonce(std::uint8_t nonce[16], std::uint32_t time, std::uint16_t expiry,
                std::uint64_t reserved) noexcept {
    store32le(nonce + 0, time);
    store16le(nonce + 4, expiry);
    store64le(nonce + 6, reserved);
    // Mix the bytes with a simple xorshift to avoid obvious patterns.
    std::uint32_t x = time ^ 0x12345678;
    for (int i = 0; i < 4; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        store32le(nonce + 12, x);
    }
}

} // unnamed namespace

// ── Public API ──────────────────────────────────────────────────────────────

bool ConnectionToken::generate(std::uint8_t out_token[SIZE],
                               const std::uint8_t secret[32],
                               std::uint32_t expiry_seconds,
                               std::uint64_t reserved_client_id,
                               std::uint32_t current_time_unix) noexcept {
    if (expiry_seconds == 0 || expiry_seconds > 86400) {
        // Clamp to reasonable range: 1 second .. 24 hours.
        return false;
    }

    std::memset(out_token, 0, SIZE);

    store32le(out_token + 0, current_time_unix);
    store16le(out_token + 4, static_cast<std::uint16_t>(expiry_seconds));
    store64le(out_token + 6, reserved_client_id);
    fill_nonce(out_token + 14, current_time_unix,
               static_cast<std::uint16_t>(expiry_seconds), reserved_client_id);
    out_token[30] = 0;
    out_token[31] = 0;

    crypto::hmac_sha256(secret, 32, out_token, 32, out_token + 32);
    return true;
}

bool ConnectionToken::validate(const std::uint8_t token[SIZE],
                               const std::uint8_t secret[32],
                               std::uint32_t current_time_unix) noexcept {
    // Verify reserved bytes are zero.
    if (token[30] != 0 || token[31] != 0)
        return false;

    // Recompute HMAC.
    std::uint8_t computed_hmac[32];
    crypto::hmac_sha256(secret, 32, token, 32, computed_hmac);
    std::uint32_t diff = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        diff |= static_cast<std::uint32_t>(computed_hmac[i] ^ token[32 + i]);
    }
    if (diff != 0)
        return false;

    // Check expiry.
    std::uint32_t issued = load32le(token + 0);
    std::uint16_t expiry = load16le(token + 4);
    if (current_time_unix < issued || current_time_unix > issued + expiry)
        return false;

    return true;
}

std::uint64_t ConnectionToken::reserved_client_id(const std::uint8_t token[SIZE]) noexcept {
    return load64le(token + 6);
}

} // namespace systems::leal::campello_net

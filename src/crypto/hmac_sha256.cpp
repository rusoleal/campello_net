#include "campello_net/crypto/hmac_sha256.hpp"

#include <cstring>

namespace systems::leal::campello_net::crypto {

namespace {

// ── SHA-256 core ────────────────────────────────────────────────────────────

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n) noexcept {
    return (x >> n) | (x << (32 - n));
}

inline std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}

inline std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline std::uint32_t ep0(std::uint32_t x) noexcept {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline std::uint32_t ep1(std::uint32_t x) noexcept {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline std::uint32_t sig0(std::uint32_t x) noexcept {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline std::uint32_t sig1(std::uint32_t x) noexcept {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

static constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

struct Sha256State {
    std::uint8_t data[64];
    std::uint32_t datalen = 0;
    std::uint64_t bitlen = 0;
    std::uint32_t state[8];

    Sha256State() noexcept {
        state[0] = 0x6a09e667;
        state[1] = 0xbb67ae85;
        state[2] = 0x3c6ef372;
        state[3] = 0xa54ff53a;
        state[4] = 0x510e527f;
        state[5] = 0x9b05688c;
        state[6] = 0x1f83d9ab;
        state[7] = 0x5be0cd19;
    }
};

inline void sha256_transform(Sha256State& ctx, const std::uint8_t* data) noexcept {
    std::uint32_t m[64];
    std::uint32_t i, j;

    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = static_cast<std::uint32_t>(data[j]) << 24 | static_cast<std::uint32_t>(data[j + 1]) << 16 |
               static_cast<std::uint32_t>(data[j + 2]) << 8 | static_cast<std::uint32_t>(data[j + 3]);
    }
    for (; i < 64; ++i) {
        m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];
    }

    std::uint32_t a = ctx.state[0];
    std::uint32_t b = ctx.state[1];
    std::uint32_t c = ctx.state[2];
    std::uint32_t d = ctx.state[3];
    std::uint32_t e = ctx.state[4];
    std::uint32_t f = ctx.state[5];
    std::uint32_t g = ctx.state[6];
    std::uint32_t h = ctx.state[7];

    for (i = 0; i < 64; ++i) {
        std::uint32_t t1 = h + ep1(e) + ch(e, f, g) + K[i] + m[i];
        std::uint32_t t2 = ep0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
    ctx.state[5] += f;
    ctx.state[6] += g;
    ctx.state[7] += h;
}

inline void sha256_update(Sha256State& ctx, const std::uint8_t* data, std::size_t len) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
        ctx.data[ctx.datalen] = data[i];
        ctx.datalen++;
        if (ctx.datalen == 64) {
            sha256_transform(ctx, ctx.data);
            ctx.bitlen += 512;
            ctx.datalen = 0;
        }
    }
}

inline void sha256_final(Sha256State& ctx, std::uint8_t hash[32]) noexcept {
    std::uint32_t i = ctx.datalen;

    if (ctx.datalen < 56) {
        ctx.data[i++] = 0x80;
        while (i < 56) {
            ctx.data[i++] = 0x00;
        }
    } else {
        ctx.data[i++] = 0x80;
        while (i < 64) {
            ctx.data[i++] = 0x00;
        }
        sha256_transform(ctx, ctx.data);
        std::memset(ctx.data, 0, 56);
    }

    ctx.bitlen += ctx.datalen * 8;
    ctx.data[63] = static_cast<std::uint8_t>(ctx.bitlen);
    ctx.data[62] = static_cast<std::uint8_t>(ctx.bitlen >> 8);
    ctx.data[61] = static_cast<std::uint8_t>(ctx.bitlen >> 16);
    ctx.data[60] = static_cast<std::uint8_t>(ctx.bitlen >> 24);
    ctx.data[59] = static_cast<std::uint8_t>(ctx.bitlen >> 32);
    ctx.data[58] = static_cast<std::uint8_t>(ctx.bitlen >> 40);
    ctx.data[57] = static_cast<std::uint8_t>(ctx.bitlen >> 48);
    ctx.data[56] = static_cast<std::uint8_t>(ctx.bitlen >> 56);
    sha256_transform(ctx, ctx.data);

    for (i = 0; i < 4; ++i) {
        hash[i] = static_cast<std::uint8_t>((ctx.state[0] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 4] = static_cast<std::uint8_t>((ctx.state[1] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 8] = static_cast<std::uint8_t>((ctx.state[2] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 12] = static_cast<std::uint8_t>((ctx.state[3] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 16] = static_cast<std::uint8_t>((ctx.state[4] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 20] = static_cast<std::uint8_t>((ctx.state[5] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 24] = static_cast<std::uint8_t>((ctx.state[6] >> (24 - i * 8)) & 0x000000ff);
        hash[i + 28] = static_cast<std::uint8_t>((ctx.state[7] >> (24 - i * 8)) & 0x000000ff);
    }
}

} // unnamed namespace

// ── Public API ──────────────────────────────────────────────────────────────

void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out_digest[32]) noexcept {
    Sha256State ctx;
    sha256_update(ctx, data, len);
    sha256_final(ctx, out_digest);
}

void hmac_sha256(const std::uint8_t* key, std::size_t key_len, const std::uint8_t* message, std::size_t message_len,
                 std::uint8_t out_mac[32]) noexcept {
    std::uint8_t k_pad[64];
    std::uint8_t tk[32];

    // If key is longer than block size, hash it first.
    if (key_len > 64) {
        sha256(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    // Inner padding: key XOR ipad (0x36)
    std::memset(k_pad, 0x36, 64);
    for (std::size_t i = 0; i < key_len; ++i) {
        k_pad[i] ^= key[i];
    }

    Sha256State inner;
    sha256_update(inner, k_pad, 64);
    sha256_update(inner, message, message_len);
    std::uint8_t inner_hash[32];
    sha256_final(inner, inner_hash);

    // Outer padding: key XOR opad (0x5c)
    std::memset(k_pad, 0x5c, 64);
    for (std::size_t i = 0; i < key_len; ++i) {
        k_pad[i] ^= key[i];
    }

    Sha256State outer;
    sha256_update(outer, k_pad, 64);
    sha256_update(outer, inner_hash, 32);
    sha256_final(outer, out_mac);
}

} // namespace systems::leal::campello_net::crypto

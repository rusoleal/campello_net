#include "campello_net/crypto/chachapoly.hpp"

#include <cstring>

namespace systems::leal::campello_net::crypto {

namespace {

// ── Little-endian helpers ───────────────────────────────────────────────────

inline std::uint32_t load32le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void store32le(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline void store64le(std::uint8_t* p, std::uint64_t v) noexcept {
    store32le(p, static_cast<std::uint32_t>(v));
    store32le(p + 4, static_cast<std::uint32_t>(v >> 32));
}

inline std::uint32_t rotl(std::uint32_t x, int n) noexcept {
    return (x << n) | (x >> (32 - n));
}

// ── ChaCha20 ────────────────────────────────────────────────────────────────

inline void qr(std::uint32_t x[16], int a, int b, int c, int d) noexcept {
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = rotl(x[d], 16);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = rotl(x[b], 12);
    x[a] += x[b];
    x[d] ^= x[a];
    x[d] = rotl(x[d], 8);
    x[c] += x[d];
    x[b] ^= x[c];
    x[b] = rotl(x[b], 7);
}

void chacha20_block(const std::uint8_t key[32], std::uint32_t counter, const std::uint8_t nonce[12],
                    std::uint8_t out[64]) noexcept {
    std::uint32_t state[16];
    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;
    state[4] = load32le(key + 0);
    state[5] = load32le(key + 4);
    state[6] = load32le(key + 8);
    state[7] = load32le(key + 12);
    state[8] = load32le(key + 16);
    state[9] = load32le(key + 20);
    state[10] = load32le(key + 24);
    state[11] = load32le(key + 28);
    state[12] = counter;
    state[13] = load32le(nonce + 0);
    state[14] = load32le(nonce + 4);
    state[15] = load32le(nonce + 8);

    std::uint32_t working[16];
    std::memcpy(working, state, sizeof(working));

    for (int i = 0; i < 10; ++i) {
        qr(working, 0, 4, 8, 12);
        qr(working, 1, 5, 9, 13);
        qr(working, 2, 6, 10, 14);
        qr(working, 3, 7, 11, 15);
        qr(working, 0, 5, 10, 15);
        qr(working, 1, 6, 11, 12);
        qr(working, 2, 7, 8, 13);
        qr(working, 3, 4, 9, 14);
    }

    for (int i = 0; i < 16; ++i) {
        working[i] += state[i];
    }

    for (int i = 0; i < 16; ++i) {
        store32le(out + i * 4, working[i]);
    }
}

void chacha20_crypt(const std::uint8_t key[32], const std::uint8_t nonce[12], std::uint32_t counter, std::uint8_t* data,
                    std::size_t len) noexcept {
    std::uint8_t block[64];
    std::size_t offset = 0;
    while (len > 0) {
        chacha20_block(key, counter, nonce, block);
        std::size_t chunk = len < 64 ? len : 64;
        for (std::size_t i = 0; i < chunk; ++i) {
            data[offset + i] ^= block[i];
        }
        ++counter;
        offset += chunk;
        len -= chunk;
    }
}

// ── Poly1305 (5×26-bit limb implementation) ─────────────────────────────────

struct Poly1305State {
    std::uint32_t r[5];   // clamped key (26-bit limbs)
    std::uint32_t h[5];   // accumulator (26-bit limbs)
    std::uint32_t pad[4]; // s (32-bit words)
    std::size_t leftover;
    std::uint8_t buffer[16];
};

void poly1305_init(Poly1305State& st, const std::uint8_t key[32]) noexcept {
    std::memset(&st, 0, sizeof(st));

    // Clamp r per RFC 8439: top 4 bits clear at bytes 3,7,11,15;
    // bottom bit clear at bytes 4,8,12.
    std::uint8_t clamped[16];
    std::memcpy(clamped, key, 16);
    clamped[3] &= 0x0f;
    clamped[7] &= 0x0f;
    clamped[11] &= 0x0f;
    clamped[15] &= 0x0f;
    clamped[4] &= 0xfe;
    clamped[8] &= 0xfe;
    clamped[12] &= 0xfe;

    std::uint32_t t0 = load32le(clamped + 0);
    std::uint32_t t1 = load32le(clamped + 4);
    std::uint32_t t2 = load32le(clamped + 8);
    std::uint32_t t3 = load32le(clamped + 12);

    st.r[0] = (t0) & 0x3ffffff;
    st.r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
    st.r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
    st.r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
    st.r[4] = (t3 >> 8) & 0x3ffffff;

    st.pad[0] = load32le(key + 16);
    st.pad[1] = load32le(key + 20);
    st.pad[2] = load32le(key + 24);
    st.pad[3] = load32le(key + 28);
}

inline void poly1305_blocks(Poly1305State& st, const std::uint8_t* m, std::size_t bytes, std::uint32_t hibit) noexcept {
    const std::uint32_t s1 = st.r[1] * 5;
    const std::uint32_t s2 = st.r[2] * 5;
    const std::uint32_t s3 = st.r[3] * 5;
    const std::uint32_t s4 = st.r[4] * 5;

    while (bytes >= 16) {
        std::uint32_t b0 = load32le(m + 0);
        std::uint32_t b1 = load32le(m + 4);
        std::uint32_t b2 = load32le(m + 8);
        std::uint32_t b3 = load32le(m + 12);

        st.h[0] += (b0 >> 0) & 0x3ffffff;
        st.h[1] += ((b0 >> 26) | (b1 << 6)) & 0x3ffffff;
        st.h[2] += ((b1 >> 20) | (b2 << 12)) & 0x3ffffff;
        st.h[3] += ((b2 >> 14) | (b3 << 18)) & 0x3ffffff;
        st.h[4] += (b3 >> 8) | hibit;

        // h *= r  (5×5 schoolbook multiply)
        std::uint64_t d0 = static_cast<std::uint64_t>(st.h[0]) * st.r[0] + static_cast<std::uint64_t>(st.h[1]) * s4 +
                           static_cast<std::uint64_t>(st.h[2]) * s3 + static_cast<std::uint64_t>(st.h[3]) * s2 +
                           static_cast<std::uint64_t>(st.h[4]) * s1;
        std::uint64_t d1 = static_cast<std::uint64_t>(st.h[0]) * st.r[1] +
                           static_cast<std::uint64_t>(st.h[1]) * st.r[0] + static_cast<std::uint64_t>(st.h[2]) * s4 +
                           static_cast<std::uint64_t>(st.h[3]) * s3 + static_cast<std::uint64_t>(st.h[4]) * s2;
        std::uint64_t d2 = static_cast<std::uint64_t>(st.h[0]) * st.r[2] +
                           static_cast<std::uint64_t>(st.h[1]) * st.r[1] +
                           static_cast<std::uint64_t>(st.h[2]) * st.r[0] + static_cast<std::uint64_t>(st.h[3]) * s4 +
                           static_cast<std::uint64_t>(st.h[4]) * s3;
        std::uint64_t d3 = static_cast<std::uint64_t>(st.h[0]) * st.r[3] +
                           static_cast<std::uint64_t>(st.h[1]) * st.r[2] +
                           static_cast<std::uint64_t>(st.h[2]) * st.r[1] +
                           static_cast<std::uint64_t>(st.h[3]) * st.r[0] + static_cast<std::uint64_t>(st.h[4]) * s4;
        std::uint64_t d4 =
            static_cast<std::uint64_t>(st.h[0]) * st.r[4] + static_cast<std::uint64_t>(st.h[1]) * st.r[3] +
            static_cast<std::uint64_t>(st.h[2]) * st.r[2] + static_cast<std::uint64_t>(st.h[3]) * st.r[1] +
            static_cast<std::uint64_t>(st.h[4]) * st.r[0];

        // carry propagation (limbs are 26-bit)
        std::uint32_t c = static_cast<std::uint32_t>(d0 >> 26);
        st.h[0] = static_cast<std::uint32_t>(d0) & 0x3ffffff;
        d1 += c;
        c = static_cast<std::uint32_t>(d1 >> 26);
        st.h[1] = static_cast<std::uint32_t>(d1) & 0x3ffffff;
        d2 += c;
        c = static_cast<std::uint32_t>(d2 >> 26);
        st.h[2] = static_cast<std::uint32_t>(d2) & 0x3ffffff;
        d3 += c;
        c = static_cast<std::uint32_t>(d3 >> 26);
        st.h[3] = static_cast<std::uint32_t>(d3) & 0x3ffffff;
        d4 += c;
        c = static_cast<std::uint32_t>(d4 >> 26);
        st.h[4] = static_cast<std::uint32_t>(d4) & 0x3ffffff;

        // final reduction: h += c * 5  (because 2^130 ≡ 5 mod p)
        st.h[0] += c * 5;
        c = (st.h[0] >> 26);
        st.h[0] &= 0x03ffffff;
        st.h[1] += c;

        m += 16;
        bytes -= 16;
    }
}

void poly1305_update(Poly1305State& st, const std::uint8_t* m, std::size_t bytes) noexcept {
    if (st.leftover) {
        std::size_t want = 16 - st.leftover;
        if (want > bytes)
            want = bytes;
        std::memcpy(st.buffer + st.leftover, m, want);
        st.leftover += want;
        m += want;
        bytes -= want;
        if (st.leftover < 16)
            return;
        poly1305_blocks(st, st.buffer, 16, 1u << 24);
        st.leftover = 0;
    }

    if (bytes >= 16) {
        std::size_t n = bytes & ~static_cast<std::size_t>(15);
        poly1305_blocks(st, m, n, 1u << 24);
        m += n;
        bytes -= n;
    }

    if (bytes) {
        std::memcpy(st.buffer + st.leftover, m, bytes);
        st.leftover += bytes;
    }
}

void poly1305_finish(Poly1305State& st, std::uint8_t mac[16]) noexcept {
    if (st.leftover) {
        std::uint8_t padded[16] = {};
        std::memcpy(padded, st.buffer, st.leftover);
        padded[st.leftover] = 1;

        std::uint32_t b0 = load32le(padded + 0);
        std::uint32_t b1 = load32le(padded + 4);
        std::uint32_t b2 = load32le(padded + 8);
        std::uint32_t b3 = load32le(padded + 12);

        st.h[0] += (b0 >> 0) & 0x3ffffff;
        st.h[1] += ((b0 >> 26) | (b1 << 6)) & 0x3ffffff;
        st.h[2] += ((b1 >> 20) | (b2 << 12)) & 0x3ffffff;
        st.h[3] += ((b2 >> 14) | (b3 << 18)) & 0x3ffffff;
        st.h[4] += (b3 >> 8); // NO hibit for partial block

        const std::uint32_t s1 = st.r[1] * 5;
        const std::uint32_t s2 = st.r[2] * 5;
        const std::uint32_t s3 = st.r[3] * 5;
        const std::uint32_t s4 = st.r[4] * 5;

        std::uint64_t d0 = static_cast<std::uint64_t>(st.h[0]) * st.r[0] + static_cast<std::uint64_t>(st.h[1]) * s4 +
                           static_cast<std::uint64_t>(st.h[2]) * s3 + static_cast<std::uint64_t>(st.h[3]) * s2 +
                           static_cast<std::uint64_t>(st.h[4]) * s1;
        std::uint64_t d1 = static_cast<std::uint64_t>(st.h[0]) * st.r[1] +
                           static_cast<std::uint64_t>(st.h[1]) * st.r[0] + static_cast<std::uint64_t>(st.h[2]) * s4 +
                           static_cast<std::uint64_t>(st.h[3]) * s3 + static_cast<std::uint64_t>(st.h[4]) * s2;
        std::uint64_t d2 = static_cast<std::uint64_t>(st.h[0]) * st.r[2] +
                           static_cast<std::uint64_t>(st.h[1]) * st.r[1] +
                           static_cast<std::uint64_t>(st.h[2]) * st.r[0] + static_cast<std::uint64_t>(st.h[3]) * s4 +
                           static_cast<std::uint64_t>(st.h[4]) * s3;
        std::uint64_t d3 = static_cast<std::uint64_t>(st.h[0]) * st.r[3] +
                           static_cast<std::uint64_t>(st.h[1]) * st.r[2] +
                           static_cast<std::uint64_t>(st.h[2]) * st.r[1] +
                           static_cast<std::uint64_t>(st.h[3]) * st.r[0] + static_cast<std::uint64_t>(st.h[4]) * s4;
        std::uint64_t d4 =
            static_cast<std::uint64_t>(st.h[0]) * st.r[4] + static_cast<std::uint64_t>(st.h[1]) * st.r[3] +
            static_cast<std::uint64_t>(st.h[2]) * st.r[2] + static_cast<std::uint64_t>(st.h[3]) * st.r[1] +
            static_cast<std::uint64_t>(st.h[4]) * st.r[0];

        std::uint32_t c = static_cast<std::uint32_t>(d0 >> 26);
        st.h[0] = static_cast<std::uint32_t>(d0) & 0x3ffffff;
        d1 += c;
        c = static_cast<std::uint32_t>(d1 >> 26);
        st.h[1] = static_cast<std::uint32_t>(d1) & 0x3ffffff;
        d2 += c;
        c = static_cast<std::uint32_t>(d2 >> 26);
        st.h[2] = static_cast<std::uint32_t>(d2) & 0x3ffffff;
        d3 += c;
        c = static_cast<std::uint32_t>(d3 >> 26);
        st.h[3] = static_cast<std::uint32_t>(d3) & 0x3ffffff;
        d4 += c;
        c = static_cast<std::uint32_t>(d4 >> 26);
        st.h[4] = static_cast<std::uint32_t>(d4) & 0x3ffffff;
        st.h[0] += c * 5;
        c = (st.h[0] >> 26);
        st.h[0] &= 0x03ffffff;
        st.h[1] += c;
    }

    // Pack low 128 bits of h into a 16-byte little-endian integer,
    // then add s (pad) and store the result as the tag.
    std::uint8_t h_packed[16];
    store32le(h_packed + 0, (st.h[0] >> 0) | (st.h[1] << 26));
    store32le(h_packed + 4, (st.h[1] >> 6) | (st.h[2] << 20));
    store32le(h_packed + 8, (st.h[2] >> 12) | (st.h[3] << 14));
    store32le(h_packed + 12, (st.h[3] >> 18) | ((st.h[4] & 0x00ffffff) << 8));

    std::uint64_t u = 0;
    for (int i = 0; i < 16; i += 4) {
        std::uint32_t word = load32le(h_packed + i);
        u += static_cast<std::uint64_t>(word) + st.pad[i / 4];
        store32le(mac + i, static_cast<std::uint32_t>(u));
        u >>= 32;
    }
}

// ── AEAD construction ───────────────────────────────────────────────────────

void aead_compute_tag(const std::uint8_t poly_key[32], const std::uint8_t* aad, std::size_t aad_len,
                      const std::uint8_t* ciphertext, std::size_t ciphertext_len, std::uint8_t tag[16]) noexcept {
    Poly1305State st;
    poly1305_init(st, poly_key);

    if (aad_len > 0) {
        poly1305_update(st, aad, aad_len);
        std::size_t pad = 16 - (aad_len & 15);
        if (pad < 16) {
            std::uint8_t zero[16] = {};
            poly1305_update(st, zero, pad);
        }
    }

    if (ciphertext_len > 0) {
        poly1305_update(st, ciphertext, ciphertext_len);
        std::size_t pad = 16 - (ciphertext_len & 15);
        if (pad < 16) {
            std::uint8_t zero[16] = {};
            poly1305_update(st, zero, pad);
        }
    }

    std::uint8_t len_block[16];
    store64le(len_block + 0, static_cast<std::uint64_t>(aad_len));
    store64le(len_block + 8, static_cast<std::uint64_t>(ciphertext_len));
    poly1305_update(st, len_block, 16);

    poly1305_finish(st, tag);
}

} // unnamed namespace

// ── Public API ──────────────────────────────────────────────────────────────

bool ChaCha20Poly1305::seal(const std::uint8_t key[KEY_SIZE], const std::uint8_t nonce[NONCE_SIZE],
                            const std::uint8_t* aad, std::size_t aad_len, const std::uint8_t* plaintext,
                            std::size_t plaintext_len, std::uint8_t* out, std::size_t out_len) {
    if (out_len < plaintext_len + TAG_SIZE)
        return false;

    // Derive one-time Poly1305 key
    std::uint8_t poly_key[64];
    chacha20_block(key, 0, nonce, poly_key);

    // Encrypt plaintext
    std::memcpy(out, plaintext, plaintext_len);
    chacha20_crypt(key, nonce, 1, out, plaintext_len);

    // Compute tag
    aead_compute_tag(poly_key, aad, aad_len, out, plaintext_len, out + plaintext_len);

    return true;
}

bool ChaCha20Poly1305::open(const std::uint8_t key[KEY_SIZE], const std::uint8_t nonce[NONCE_SIZE],
                            const std::uint8_t* aad, std::size_t aad_len, const std::uint8_t* ciphertext,
                            std::size_t ciphertext_len, std::uint8_t* out, std::size_t out_len) {
    if (ciphertext_len < TAG_SIZE)
        return false;
    if (out_len < ciphertext_len - TAG_SIZE)
        return false;

    std::size_t encrypted_len = ciphertext_len - TAG_SIZE;
    const std::uint8_t* tag = ciphertext + encrypted_len;

    // Derive one-time Poly1305 key
    std::uint8_t poly_key[64];
    chacha20_block(key, 0, nonce, poly_key);

    // Verify tag (constant-time comparison)
    std::uint8_t computed_tag[16];
    aead_compute_tag(poly_key, aad, aad_len, ciphertext, encrypted_len, computed_tag);

    std::uint32_t diff = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        diff |= static_cast<std::uint32_t>(computed_tag[i] ^ tag[i]);
    }
    if (diff != 0)
        return false;

    // Decrypt
    std::memcpy(out, ciphertext, encrypted_len);
    chacha20_crypt(key, nonce, 1, out, encrypted_len);

    return true;
}

void ChaCha20Poly1305::derive_keys(const std::uint8_t master_key[KEY_SIZE], std::uint8_t client_to_server[KEY_SIZE],
                                   std::uint8_t server_to_client[KEY_SIZE]) {
    std::uint8_t block[64];
    std::uint8_t zero_nonce[NONCE_SIZE] = {};
    chacha20_block(master_key, 0, zero_nonce, block);
    std::memcpy(client_to_server, block, KEY_SIZE);
    std::memcpy(server_to_client, block + KEY_SIZE, KEY_SIZE);
}

} // namespace systems::leal::campello_net::crypto

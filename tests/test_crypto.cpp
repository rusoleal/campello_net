#include <catch2/catch_test_macros.hpp>

#include "campello_net/crypto/chachapoly.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using systems::leal::campello_net::crypto::ChaCha20Poly1305;

TEST_CASE("ChaCha20-Poly1305 RFC 8439 test vector", "[crypto]") {
    // RFC 8439 Section 2.8.2 Test Vector
    const std::array<std::uint8_t, 32> key = {
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    };
    const std::array<std::uint8_t, 12> nonce = {
        0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43,
        0x44, 0x45, 0x46, 0x47,
    };
    const std::array<std::uint8_t, 114> plaintext = {
        0x4c, 0x61, 0x64, 0x69, 0x65, 0x73, 0x20, 0x61,
        0x6e, 0x64, 0x20, 0x47, 0x65, 0x6e, 0x74, 0x6c,
        0x65, 0x6d, 0x65, 0x6e, 0x20, 0x6f, 0x66, 0x20,
        0x74, 0x68, 0x65, 0x20, 0x63, 0x6c, 0x61, 0x73,
        0x73, 0x20, 0x6f, 0x66, 0x20, 0x27, 0x39, 0x39,
        0x3a, 0x20, 0x49, 0x66, 0x20, 0x49, 0x20, 0x63,
        0x6f, 0x75, 0x6c, 0x64, 0x20, 0x6f, 0x66, 0x66,
        0x65, 0x72, 0x20, 0x79, 0x6f, 0x75, 0x20, 0x6f,
        0x6e, 0x6c, 0x79, 0x20, 0x6f, 0x6e, 0x65, 0x20,
        0x74, 0x69, 0x70, 0x20, 0x66, 0x6f, 0x72, 0x20,
        0x74, 0x68, 0x65, 0x20, 0x66, 0x75, 0x74, 0x75,
        0x72, 0x65, 0x2c, 0x20, 0x73, 0x75, 0x6e, 0x73,
        0x63, 0x72, 0x65, 0x65, 0x6e, 0x20, 0x77, 0x6f,
        0x75, 0x6c, 0x64, 0x20, 0x62, 0x65, 0x20, 0x69,
        0x74, 0x2e,
    };
    const std::array<std::uint8_t, 12> aad = {
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3,
        0xc4, 0xc5, 0xc6, 0xc7,
    };

    // Expected ciphertext
    const std::array<std::uint8_t, 114> expected_ciphertext = {
        0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
        0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
        0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
        0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6,
        0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12,
        0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
        0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29,
        0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36,
        0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
        0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58,
        0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94,
        0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
        0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d,
        0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b,
        0x61, 0x16,
    };
    const std::array<std::uint8_t, 16> expected_tag = {
        0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
        0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91,
    };

    SECTION("seal produces correct ciphertext and tag") {
        std::vector<std::uint8_t> out(plaintext.size() + ChaCha20Poly1305::TAG_SIZE);
        REQUIRE(ChaCha20Poly1305::seal(
            key.data(), nonce.data(),
            aad.data(), aad.size(),
            plaintext.data(), plaintext.size(),
            out.data(), out.size()));

        std::vector<std::uint8_t> ciphertext(out.begin(), out.begin() + plaintext.size());
        std::vector<std::uint8_t> tag(out.begin() + plaintext.size(), out.end());

        REQUIRE(ciphertext == std::vector<std::uint8_t>(expected_ciphertext.begin(), expected_ciphertext.end()));
        REQUIRE(tag == std::vector<std::uint8_t>(expected_tag.begin(), expected_tag.end()));
    }

    SECTION("open decrypts correctly") {
        // Build combined ciphertext + tag
        std::vector<std::uint8_t> combined;
        combined.insert(combined.end(), expected_ciphertext.begin(), expected_ciphertext.end());
        combined.insert(combined.end(), expected_tag.begin(), expected_tag.end());

        std::vector<std::uint8_t> decrypted(plaintext.size());
        REQUIRE(ChaCha20Poly1305::open(
            key.data(), nonce.data(),
            aad.data(), aad.size(),
            combined.data(), combined.size(),
            decrypted.data(), decrypted.size()));

        REQUIRE(decrypted == std::vector<std::uint8_t>(plaintext.begin(), plaintext.end()));
    }

    SECTION("open rejects tampered ciphertext") {
        std::vector<std::uint8_t> combined;
        combined.insert(combined.end(), expected_ciphertext.begin(), expected_ciphertext.end());
        combined.insert(combined.end(), expected_tag.begin(), expected_tag.end());
        combined[0] ^= 0xFF; // tamper

        std::vector<std::uint8_t> decrypted(plaintext.size());
        REQUIRE_FALSE(ChaCha20Poly1305::open(
            key.data(), nonce.data(),
            aad.data(), aad.size(),
            combined.data(), combined.size(),
            decrypted.data(), decrypted.size()));
    }

    SECTION("open rejects tampered tag") {
        std::vector<std::uint8_t> combined;
        combined.insert(combined.end(), expected_ciphertext.begin(), expected_ciphertext.end());
        combined.insert(combined.end(), expected_tag.begin(), expected_tag.end());
        combined[combined.size() - 1] ^= 0xFF; // tamper tag

        std::vector<std::uint8_t> decrypted(plaintext.size());
        REQUIRE_FALSE(ChaCha20Poly1305::open(
            key.data(), nonce.data(),
            aad.data(), aad.size(),
            combined.data(), combined.size(),
            decrypted.data(), decrypted.size()));
    }
}

TEST_CASE("ChaCha20-Poly1305 empty plaintext", "[crypto]") {
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 12> nonce{};

    std::uint8_t out[ChaCha20Poly1305::TAG_SIZE];
    REQUIRE(ChaCha20Poly1305::seal(key.data(), nonce.data(), nullptr, 0, nullptr, 0, out, sizeof(out)));

    std::uint8_t decrypted[1];
    REQUIRE(ChaCha20Poly1305::open(key.data(), nonce.data(), nullptr, 0, out, sizeof(out), decrypted, sizeof(decrypted)));
}

TEST_CASE("ChaCha20-Poly1305 derive_keys", "[crypto]") {
    std::array<std::uint8_t, 32> master_key{};
    for (std::size_t i = 0; i < 32; ++i) master_key[i] = static_cast<std::uint8_t>(i);

    std::array<std::uint8_t, 32> c2s{};
    std::array<std::uint8_t, 32> s2c{};
    ChaCha20Poly1305::derive_keys(master_key.data(), c2s.data(), s2c.data());

    // Keys should be different from each other and from master
    REQUIRE(c2s != master_key);
    REQUIRE(s2c != master_key);
    REQUIRE(c2s != s2c);
}

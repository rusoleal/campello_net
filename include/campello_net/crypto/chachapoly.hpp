#pragma once

#include <cstddef>
#include <cstdint>

namespace systems::leal::campello_net::crypto {

/// @brief AEAD ChaCha20-Poly1305 (IETF variant, RFC 8439).
///
/// Zero-allocation, zero-dependencies implementation.
/// Suitable for encrypting network payloads in the EncryptedTransport wrapper.
class ChaCha20Poly1305 {
public:
    static constexpr std::size_t KEY_SIZE = 32;
    static constexpr std::size_t NONCE_SIZE = 12;
    static constexpr std::size_t TAG_SIZE = 16;

    /// @brief Encrypt and authenticate.
    ///
    /// @param key 32-byte secret key.
    /// @param nonce 12-byte unique nonce (must never repeat for same key).
    /// @param aad Additional authenticated data (may be nullptr if aad_len == 0).
    /// @param aad_len Length of AAD.
    /// @param plaintext Data to encrypt.
    /// @param plaintext_len Length of plaintext.
    /// @param out Output buffer. Must be at least plaintext_len + TAG_SIZE bytes.
    /// @param out_len Capacity of out buffer.
    /// @return true on success, false if out_len is too small.
    static bool seal(const std::uint8_t key[KEY_SIZE], const std::uint8_t nonce[NONCE_SIZE], const std::uint8_t* aad,
                     std::size_t aad_len, const std::uint8_t* plaintext, std::size_t plaintext_len, std::uint8_t* out,
                     std::size_t out_len);

    /// @brief Decrypt and verify.
    ///
    /// @param key 32-byte secret key.
    /// @param nonce 12-byte nonce.
    /// @param aad Additional authenticated data (may be nullptr if aad_len == 0).
    /// @param aad_len Length of AAD.
    /// @param ciphertext Encrypted data including the 16-byte tag at the end.
    /// @param ciphertext_len Length of ciphertext (must be >= TAG_SIZE).
    /// @param out Output buffer for plaintext. Must be at least ciphertext_len - TAG_SIZE bytes.
    /// @param out_len Capacity of out buffer.
    /// @return true if decryption and authentication succeed, false otherwise.
    static bool open(const std::uint8_t key[KEY_SIZE], const std::uint8_t nonce[NONCE_SIZE], const std::uint8_t* aad,
                     std::size_t aad_len, const std::uint8_t* ciphertext, std::size_t ciphertext_len, std::uint8_t* out,
                     std::size_t out_len);

    /// @brief Derive two directional session keys from a single master key.
    ///
    /// Uses one ChaCha20 block (counter=0, nonce=0) to generate 64 bytes of
    /// key material. The first 32 bytes become the client→server key;
    /// the second 32 bytes become the server→client key.
    static void derive_keys(const std::uint8_t master_key[KEY_SIZE], std::uint8_t client_to_server[KEY_SIZE],
                            std::uint8_t server_to_client[KEY_SIZE]);
};

} // namespace systems::leal::campello_net::crypto

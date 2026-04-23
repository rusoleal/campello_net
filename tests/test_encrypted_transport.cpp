#include <array>
#include <campello_net/transport/encrypted_transport.hpp>
#include <campello_net/transport/loopback_transport.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string_view>

using namespace systems::leal::campello_net::transport;

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::array<std::uint8_t, 32> make_test_key(std::uint8_t seed) {
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < 32; ++i)
        key[i] = static_cast<std::uint8_t>(seed + i);
    return key;
}

static void store_counter_le(std::uint8_t* dst, std::uint64_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v);
    dst[1] = static_cast<std::uint8_t>(v >> 8);
    dst[2] = static_cast<std::uint8_t>(v >> 16);
    dst[3] = static_cast<std::uint8_t>(v >> 24);
    dst[4] = static_cast<std::uint8_t>(v >> 32);
    dst[5] = static_cast<std::uint8_t>(v >> 40);
    dst[6] = static_cast<std::uint8_t>(v >> 48);
    dst[7] = static_cast<std::uint8_t>(v >> 56);
}

// ── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("EncryptedTransport client sends to server", "[transport][encrypted]") {
    auto hub = std::make_shared<LoopbackHub>();
    auto server_inner = std::make_unique<LoopbackTransport>(hub);
    auto client_inner = std::make_unique<LoopbackTransport>(hub);

    Address server_addr("127.0.0.1", 21000);
    REQUIRE(server_inner->bind(server_addr));
    REQUIRE(client_inner->connect(server_addr));

    auto key = make_test_key(0x42);
    EncryptedTransport server(std::move(server_inner), key);
    EncryptedTransport client(std::move(client_inner), key);

    const char* msg = "secret hello from client";
    REQUIRE(client.send(reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1,
                        PacketReliability::ReliableOrdered));

    server.poll();

    std::uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(server.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == std::strlen(msg) + 1);
    REQUIRE(std::string_view(reinterpret_cast<char*>(buffer), len - 1) == msg);
}

TEST_CASE("EncryptedTransport server send_to targets client", "[transport][encrypted]") {
    auto hub = std::make_shared<LoopbackHub>();
    auto server_inner = std::make_unique<LoopbackTransport>(hub);
    auto client_inner = std::make_unique<LoopbackTransport>(hub);

    Address server_addr("127.0.0.1", 21001);
    REQUIRE(server_inner->bind(server_addr));
    REQUIRE(client_inner->connect(server_addr));

    auto key = make_test_key(0x55);
    EncryptedTransport server(std::move(server_inner), key);
    EncryptedTransport client(std::move(client_inner), key);

    // Client sends first so server learns the client's address.
    client.send(reinterpret_cast<const std::uint8_t*>("ping"), 4, PacketReliability::ReliableOrdered);
    server.poll();

    std::uint8_t discard[256] = {};
    std::size_t discard_len = 0;
    Address client_addr;
    REQUIRE(server.pop_receive(discard, sizeof(discard), discard_len, client_addr));

    // Server now sends targeted reply.
    const char* reply = "secret reply from server";
    REQUIRE(server.send_to(client_addr, reinterpret_cast<const std::uint8_t*>(reply), std::strlen(reply) + 1,
                           PacketReliability::ReliableOrdered));

    client.poll();

    std::uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(client.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == std::strlen(reply) + 1);
    REQUIRE(std::string_view(reinterpret_cast<char*>(buffer), len - 1) == reply);
}

#include "campello_net/crypto/chachapoly.hpp"

using systems::leal::campello_net::crypto::ChaCha20Poly1305;

TEST_CASE("EncryptedTransport rejects tampered packet", "[transport][encrypted]") {
    auto hub = std::make_shared<LoopbackHub>();
    auto server_inner = std::make_unique<LoopbackTransport>(hub);

    Address server_addr("127.0.0.1", 21003);
    server_inner->bind(server_addr);

    auto key = make_test_key(0xBB);
    EncryptedTransport server(std::move(server_inner), key);

    // Derive directional keys (same as EncryptedTransport constructor does).
    std::array<std::uint8_t, 32> c2s_key{};
    std::array<std::uint8_t, 32> s2c_key{};
    ChaCha20Poly1305::derive_keys(key.data(), c2s_key.data(), s2c_key.data());

    // Build a valid encrypted payload manually.
    std::uint8_t nonce[ChaCha20Poly1305::NONCE_SIZE] = {};
    nonce[0] = 0x00; // DIR_CLIENT_TO_SERVER
    // counter = 0 in bytes 4..11

    const char* plaintext = "tamper test";
    std::size_t pt_len = std::strlen(plaintext) + 1;
    std::vector<std::uint8_t> ciphertext(pt_len + ChaCha20Poly1305::TAG_SIZE);
    REQUIRE(ChaCha20Poly1305::seal(c2s_key.data(), nonce, nullptr, 0, reinterpret_cast<const std::uint8_t*>(plaintext),
                                   pt_len, ciphertext.data(), ciphertext.size()));

    // Prepend nonce to form the on-wire format.
    std::vector<std::uint8_t> packet;
    packet.insert(packet.end(), nonce, nonce + ChaCha20Poly1305::NONCE_SIZE);
    packet.insert(packet.end(), ciphertext.begin(), ciphertext.end());

    // Send the valid packet raw to the server.
    {
        LoopbackTransport injector(hub);
        injector.connect(server_addr);
        REQUIRE(injector.send(packet.data(), packet.size(), PacketReliability::ReliableOrdered));
    }

    server.poll();

    std::uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(server.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == pt_len);
    REQUIRE(std::memcmp(buffer, plaintext, pt_len) == 0);

    // Now tamper with the ciphertext and send again (counter 1).
    nonce[11] = 1; // bump counter so it's not a replay
    std::vector<std::uint8_t> ciphertext2(pt_len + ChaCha20Poly1305::TAG_SIZE);
    REQUIRE(ChaCha20Poly1305::seal(c2s_key.data(), nonce, nullptr, 0, reinterpret_cast<const std::uint8_t*>(plaintext),
                                   pt_len, ciphertext2.data(), ciphertext2.size()));

    // Corrupt a byte in the middle of the ciphertext.
    ciphertext2[5] ^= 0xFF;

    std::vector<std::uint8_t> packet2;
    packet2.insert(packet2.end(), nonce, nonce + ChaCha20Poly1305::NONCE_SIZE);
    packet2.insert(packet2.end(), ciphertext2.begin(), ciphertext2.end());

    {
        LoopbackTransport injector(hub);
        injector.connect(server_addr);
        REQUIRE(injector.send(packet2.data(), packet2.size(), PacketReliability::ReliableOrdered));
    }

    server.poll();

    REQUIRE_FALSE(server.pop_receive(buffer, sizeof(buffer), len, sender));
}

TEST_CASE("EncryptedTransport rejects replayed counter", "[transport][encrypted]") {
    auto hub = std::make_shared<LoopbackHub>();
    auto server_inner = std::make_unique<LoopbackTransport>(hub);

    Address server_addr("127.0.0.1", 21004);
    server_inner->bind(server_addr);

    auto key = make_test_key(0xCC);
    EncryptedTransport server(std::move(server_inner), key);

    std::array<std::uint8_t, 32> c2s_key{};
    std::array<std::uint8_t, 32> s2c_key{};
    ChaCha20Poly1305::derive_keys(key.data(), c2s_key.data(), s2c_key.data());

    // Build two encrypted packets with the SAME counter.
    std::uint8_t nonce[ChaCha20Poly1305::NONCE_SIZE] = {};
    nonce[0] = 0x00; // DIR_CLIENT_TO_SERVER
    // counter = 7 in bytes 4..11
    nonce[4] = 7;

    const char* plaintext = "replay test";
    std::size_t pt_len = std::strlen(plaintext) + 1;

    std::vector<std::uint8_t> ciphertext(pt_len + ChaCha20Poly1305::TAG_SIZE);
    REQUIRE(ChaCha20Poly1305::seal(c2s_key.data(), nonce, nullptr, 0, reinterpret_cast<const std::uint8_t*>(plaintext),
                                   pt_len, ciphertext.data(), ciphertext.size()));

    std::vector<std::uint8_t> packet;
    packet.insert(packet.end(), nonce, nonce + ChaCha20Poly1305::NONCE_SIZE);
    packet.insert(packet.end(), ciphertext.begin(), ciphertext.end());

    // Send first copy.
    LoopbackTransport injector(hub);
    injector.connect(server_addr);
    REQUIRE(injector.send(packet.data(), packet.size(), PacketReliability::ReliableOrdered));

    server.poll();

    std::uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(server.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == pt_len);

    // Send exact same packet again (same counter) from same injector.
    REQUIRE(injector.send(packet.data(), packet.size(), PacketReliability::ReliableOrdered));

    server.poll();

    REQUIRE_FALSE(server.pop_receive(buffer, sizeof(buffer), len, sender));
}

TEST_CASE("EncryptedTransport rejects old counter outside window", "[transport][encrypted]") {
    auto hub = std::make_shared<LoopbackHub>();
    auto server_inner = std::make_unique<LoopbackTransport>(hub);

    Address server_addr("127.0.0.1", 21005);
    server_inner->bind(server_addr);

    auto key = make_test_key(0xDD);
    EncryptedTransport server(std::move(server_inner), key);

    std::array<std::uint8_t, 32> c2s_key{};
    std::array<std::uint8_t, 32> s2c_key{};
    ChaCha20Poly1305::derive_keys(key.data(), c2s_key.data(), s2c_key.data());

    const char* plaintext = "window test";
    std::size_t pt_len = std::strlen(plaintext) + 1;

    // Send packet with counter = 100.
    std::uint8_t nonce[ChaCha20Poly1305::NONCE_SIZE] = {};
    nonce[0] = 0x00;
    store_counter_le(nonce + 4, 100);

    std::vector<std::uint8_t> ciphertext(pt_len + ChaCha20Poly1305::TAG_SIZE);
    REQUIRE(ChaCha20Poly1305::seal(c2s_key.data(), nonce, nullptr, 0, reinterpret_cast<const std::uint8_t*>(plaintext),
                                   pt_len, ciphertext.data(), ciphertext.size()));

    std::vector<std::uint8_t> packet;
    packet.insert(packet.end(), nonce, nonce + ChaCha20Poly1305::NONCE_SIZE);
    packet.insert(packet.end(), ciphertext.begin(), ciphertext.end());

    LoopbackTransport injector(hub);
    injector.connect(server_addr);
    REQUIRE(injector.send(packet.data(), packet.size(), PacketReliability::ReliableOrdered));

    server.poll();

    std::uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(server.pop_receive(buffer, sizeof(buffer), len, sender));

    // Now send packet with counter = 10 (way behind window of 64) from same injector.
    store_counter_le(nonce + 4, 10);
    std::vector<std::uint8_t> ciphertext2(pt_len + ChaCha20Poly1305::TAG_SIZE);
    REQUIRE(ChaCha20Poly1305::seal(c2s_key.data(), nonce, nullptr, 0, reinterpret_cast<const std::uint8_t*>(plaintext),
                                   pt_len, ciphertext2.data(), ciphertext2.size()));

    std::vector<std::uint8_t> packet2;
    packet2.insert(packet2.end(), nonce, nonce + ChaCha20Poly1305::NONCE_SIZE);
    packet2.insert(packet2.end(), ciphertext2.begin(), ciphertext2.end());

    REQUIRE(injector.send(packet2.data(), packet2.size(), PacketReliability::ReliableOrdered));

    server.poll();

    REQUIRE_FALSE(server.pop_receive(buffer, sizeof(buffer), len, sender));
}

TEST_CASE("EncryptedTransport derive_keys produces different keys", "[transport][encrypted]") {
    std::array<std::uint8_t, 32> master = make_test_key(0xEE);
    std::array<std::uint8_t, 32> c2s{};
    std::array<std::uint8_t, 32> s2c{};

    ChaCha20Poly1305::derive_keys(master.data(), c2s.data(), s2c.data());

    REQUIRE(c2s != master);
    REQUIRE(s2c != master);
    REQUIRE(c2s != s2c);
}

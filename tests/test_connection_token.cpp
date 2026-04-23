#include <array>
#include <campello_net/connection_token.hpp>
#include <campello_net/crypto/hmac_sha256.hpp>
#include <campello_net/network_manager.hpp>
#include <campello_net/transport/loopback_transport.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::crypto;
using namespace systems::leal::campello_net::transport;

// ── HMAC-SHA256 unit tests ──────────────────────────────────────────────────

TEST_CASE("HMAC-SHA256 against RFC 4231 test vectors", "[crypto][hmac]") {
    // Test vector 1: key = "key", message = "The quick brown fox jumps over the lazy dog"
    const std::uint8_t key1[] = {'k', 'e', 'y'};
    const std::uint8_t msg1[] = "The quick brown fox jumps over the lazy dog";
    std::uint8_t mac1[32];
    hmac_sha256(key1, 3, msg1, 43, mac1);

    const std::uint8_t expected1[32] = {
        0xf7, 0xbc, 0x83, 0xf4, 0x30, 0x53, 0x84, 0x24, 0xb1, 0x32, 0x98, 0xe6, 0xaa, 0x6f, 0xb1, 0x43,
        0xef, 0x4d, 0x59, 0xa1, 0x49, 0x46, 0x17, 0x59, 0x97, 0x47, 0x9d, 0xbc, 0x2d, 0x1a, 0x3c, 0xd8,
    };
    REQUIRE(std::memcmp(mac1, expected1, 32) == 0);

    // Test vector 2: empty message
    const std::uint8_t key2[] = {'k', 'e', 'y'};
    std::uint8_t mac2[32];
    hmac_sha256(key2, 3, nullptr, 0, mac2);

    const std::uint8_t expected2[32] = {
        0x5d, 0x5d, 0x13, 0x95, 0x63, 0xc9, 0x5b, 0x59, 0x67, 0xb9, 0xbd, 0x9a, 0x8c, 0x9b, 0x23, 0x3a,
        0x9d, 0xed, 0xb4, 0x50, 0x72, 0x79, 0x4c, 0xd2, 0x32, 0xdc, 0x1b, 0x74, 0x83, 0x26, 0x07, 0xd0,
    };
    REQUIRE(std::memcmp(mac2, expected2, 32) == 0);
}

TEST_CASE("SHA-256 empty input", "[crypto][sha256]") {
    std::uint8_t digest[32];
    sha256(nullptr, 0, digest);

    const std::uint8_t expected[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
    };
    REQUIRE(std::memcmp(digest, expected, 32) == 0);
}

// ── ConnectionToken unit tests ──────────────────────────────────────────────

TEST_CASE("ConnectionToken generate and validate roundtrip", "[token]") {
    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    std::uint8_t token[ConnectionToken::SIZE];
    REQUIRE(ConnectionToken::generate(token, secret.data(), 300, 42, 1000));
    REQUIRE(ConnectionToken::validate(token, secret.data(), 1000));
    REQUIRE(ConnectionToken::validate(token, secret.data(), 1299)); // edge of expiry
    REQUIRE(ConnectionToken::reserved_client_id(token) == 42);
}

TEST_CASE("ConnectionToken rejects expired token", "[token]") {
    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    std::uint8_t token[ConnectionToken::SIZE];
    REQUIRE(ConnectionToken::generate(token, secret.data(), 10, 0, 1000));
    REQUIRE(ConnectionToken::validate(token, secret.data(), 1000));
    REQUIRE_FALSE(ConnectionToken::validate(token, secret.data(), 1011)); // expired
}

TEST_CASE("ConnectionToken rejects wrong secret", "[token]") {
    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    std::uint8_t token[ConnectionToken::SIZE];
    REQUIRE(ConnectionToken::generate(token, secret.data(), 60, 0, 1000));

    std::array<std::uint8_t, 32> wrong_secret{};
    wrong_secret[0] = 0xFF;
    REQUIRE_FALSE(ConnectionToken::validate(token, wrong_secret.data(), 1000));
}

TEST_CASE("ConnectionToken rejects tampered payload", "[token]") {
    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    std::uint8_t token[ConnectionToken::SIZE];
    REQUIRE(ConnectionToken::generate(token, secret.data(), 60, 0, 1000));

    token[0] ^= 0xFF; // tamper timestamp
    REQUIRE_FALSE(ConnectionToken::validate(token, secret.data(), 1000));
}

TEST_CASE("ConnectionToken rejects non-zero reserved bytes", "[token]") {
    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    std::uint8_t token[ConnectionToken::SIZE];
    REQUIRE(ConnectionToken::generate(token, secret.data(), 60, 0, 1000));

    token[30] = 0xAB;
    REQUIRE_FALSE(ConnectionToken::validate(token, secret.data(), 1000));
}

// ── NetworkManager integration tests ────────────────────────────────────────

TEST_CASE("Client with valid token connects to server", "[token][integration]") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    NetworkManager client;

    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    NetworkManager::Config server_cfg;
    server_cfg.mode = NetworkManager::Mode::Server;
    server_cfg.bind_address = Address("127.0.0.1", 22000);
    server_cfg.connection_token_secret = secret;
    server_cfg.require_connection_token = true;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));
    REQUIRE(server.start(server_cfg));

    std::uint8_t token[ConnectionToken::SIZE];
    REQUIRE(server.generate_connection_token(token, 60));

    NetworkManager::Config client_cfg;
    client_cfg.mode = NetworkManager::Mode::Client;
    client_cfg.server_address = Address("127.0.0.1", 22000);
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.set_connection_token(token);
    REQUIRE(client.start(client_cfg));

    // Poll until connected
    bool client_connected = false;
    client.on_client_connected([&](ClientId) {
        client_connected = true;
    });

    for (int i = 0; i < 20; ++i) {
        server.poll();
        client.poll();
    }

    REQUIRE(client_connected);
}

TEST_CASE("Client without token rejected when required", "[token][integration]") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    NetworkManager client;

    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    NetworkManager::Config server_cfg;
    server_cfg.mode = NetworkManager::Mode::Server;
    server_cfg.bind_address = Address("127.0.0.1", 22001);
    server_cfg.connection_token_secret = secret;
    server_cfg.require_connection_token = true;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));
    REQUIRE(server.start(server_cfg));

    NetworkManager::Config client_cfg;
    client_cfg.mode = NetworkManager::Mode::Client;
    client_cfg.server_address = Address("127.0.0.1", 22001);
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    // No token set
    REQUIRE(client.start(client_cfg));

    bool client_connected = false;
    client.on_client_connected([&](ClientId) {
        client_connected = true;
    });

    for (int i = 0; i < 20; ++i) {
        server.poll();
        client.poll();
    }

    REQUIRE_FALSE(client_connected);
}

TEST_CASE("Client with expired token rejected", "[token][integration]") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    NetworkManager client;

    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < 32; ++i)
        secret[i] = static_cast<std::uint8_t>(i + 1);

    NetworkManager::Config server_cfg;
    server_cfg.mode = NetworkManager::Mode::Server;
    server_cfg.bind_address = Address("127.0.0.1", 22002);
    server_cfg.connection_token_secret = secret;
    server_cfg.require_connection_token = true;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));
    REQUIRE(server.start(server_cfg));

    // Generate an already-expired token (timestamp in the past).
    std::uint8_t token[ConnectionToken::SIZE];
    REQUIRE(
        ConnectionToken::generate(token, secret.data(), 1, 0,
                                  static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                                 .count() -
                                                             10)));

    NetworkManager::Config client_cfg;
    client_cfg.mode = NetworkManager::Mode::Client;
    client_cfg.server_address = Address("127.0.0.1", 22002);
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.set_connection_token(token);
    REQUIRE(client.start(client_cfg));

    bool client_connected = false;
    client.on_client_connected([&](ClientId) {
        client_connected = true;
    });

    for (int i = 0; i < 20; ++i) {
        server.poll();
        client.poll();
    }

    REQUIRE_FALSE(client_connected);
}

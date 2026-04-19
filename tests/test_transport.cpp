#include <campello_net/transport/loopback_transport.hpp>
#include <campello_net/transport/udp_transport.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace systems::leal::campello_net::transport;

TEST_CASE("Address constructs from port and string", "[transport][address]") {
    Address a(12345);
    REQUIRE(a.is_valid());
    REQUIRE(a.port() == 12345);

    Address b("127.0.0.1", 54321);
    REQUIRE(b.is_valid());
    REQUIRE(b.port() == 54321);
    REQUIRE(b.ip() == "127.0.0.1");
}

TEST_CASE("PacketHeader serializes and deserializes", "[transport][packet]") {
    PacketHeader hdr{};
    hdr.packet_type = 1;
    hdr.set_reliability(PacketReliability::ReliableOrdered);
    hdr.set_channel(2);
    hdr.sequence = 42;
    hdr.ack = 7;
    hdr.ack_bits = 0xFF00FF00;
    hdr.payload_len = 100;
    hdr.frag_index = 1;
    hdr.frag_count = 3;

    uint8_t buffer[PacketHeader::SIZE] = {};
    REQUIRE(hdr.serialize(buffer, sizeof(buffer)));

    PacketHeader decoded{};
    REQUIRE(decoded.deserialize(buffer, sizeof(buffer)));
    REQUIRE(decoded.packet_type == 1);
    REQUIRE(decoded.reliability() == PacketReliability::ReliableOrdered);
    REQUIRE(decoded.channel() == 2);
    REQUIRE(decoded.sequence == 42);
    REQUIRE(decoded.ack == 7);
    REQUIRE(decoded.ack_bits == 0xFF00FF00);
    REQUIRE(decoded.payload_len == 100);
    REQUIRE(decoded.frag_index == 1);
    REQUIRE(decoded.frag_count == 3);
}

TEST_CASE("Two transports exchange unreliable packets", "[transport]") {
    Address server_addr(17777);
    UdpTransport server;
    UdpTransport client;

    REQUIRE(server.bind(server_addr));
    REQUIRE(client.connect(Address("127.0.0.1", 17777)));

    for (int i = 0; i < 20; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(client.is_connected());

    const char* msg = "hello unreliable";
    REQUIRE(client.send(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg), PacketReliability::Unreliable));

    for (int i = 0; i < 20; ++i) {
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(server.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == std::strlen(msg));
    REQUIRE(std::string_view(reinterpret_cast<char*>(buffer), len) == msg);
}

TEST_CASE("Reliable ordered delivery", "[transport]") {
    Address server_addr(17778);
    UdpTransport server;
    UdpTransport client;

    REQUIRE(server.bind(server_addr));
    REQUIRE(client.connect(Address("127.0.0.1", 17778)));

    for (int i = 0; i < 20; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(client.is_connected());

    for (int i = 0; i < 5; ++i) {
        std::string msg = "packet " + std::to_string(i);
        REQUIRE(
            client.send(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), PacketReliability::ReliableOrdered));
    }

    std::vector<std::string> received;
    for (int attempt = 0; attempt < 200 && received.size() < 5; ++attempt) {
        server.poll();
        client.poll();

        uint8_t buffer[256] = {};
        std::size_t len = 0;
        Address sender;
        while (server.pop_receive(buffer, sizeof(buffer), len, sender)) {
            received.emplace_back(reinterpret_cast<char*>(buffer), len);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(received.size() == 5);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(received[static_cast<std::size_t>(i)] == "packet " + std::to_string(i));
    }
}

TEST_CASE("Large message fragmentation and reassembly", "[transport]") {
    Address server_addr(17779);
    UdpTransport server;
    UdpTransport client;

    REQUIRE(server.bind(server_addr));
    REQUIRE(client.connect(Address("127.0.0.1", 17779)));

    for (int i = 0; i < 20; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(client.is_connected());

    std::vector<uint8_t> large_payload(MAX_PAYLOAD_SIZE * 3 + 100, 0xAB);
    REQUIRE(client.send(large_payload.data(), large_payload.size(), PacketReliability::Unreliable));

    std::vector<uint8_t> received;
    for (int attempt = 0; attempt < 200 && received.empty(); ++attempt) {
        server.poll();
        client.poll();

        uint8_t buffer[8192] = {};
        std::size_t len = 0;
        Address sender;
        while (server.pop_receive(buffer, sizeof(buffer), len, sender)) {
            received.assign(buffer, buffer + len);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(received.size() == large_payload.size());
    REQUIRE(received == large_payload);
}

TEST_CASE("RTT is measured after reliable exchange", "[transport]") {
    Address server_addr(17780);
    UdpTransport server;
    UdpTransport client;

    REQUIRE(server.bind(server_addr));
    REQUIRE(client.connect(Address("127.0.0.1", 17780)));

    for (int i = 0; i < 20; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(client.is_connected());

    std::string msg = "ping";
    client.send(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), PacketReliability::ReliableOrdered);

    for (int i = 0; i < 100; ++i) {
        server.poll();
        client.poll();

        uint8_t buffer[256] = {};
        std::size_t len = 0;
        Address sender;
        while (server.pop_receive(buffer, sizeof(buffer), len, sender)) {
            server.send_to(sender, buffer, len, PacketReliability::ReliableOrdered);
        }
        while (client.pop_receive(buffer, sizeof(buffer), len, sender)) {
            // echoed back
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    float rtt = client.rtt();
    REQUIRE(rtt > 0.0f);
    REQUIRE(rtt < 0.5f);
}

// ── LoopbackTransport tests ─────────────────────────────────────────────────

TEST_CASE("LoopbackTransport binds and connects instantly", "[transport][loopback]") {
    auto hub = std::make_shared<LoopbackHub>();
    LoopbackTransport server(hub);
    LoopbackTransport client(hub);

    Address server_addr("127.0.0.1", 20000);
    REQUIRE(server.bind(server_addr));
    REQUIRE(client.connect(server_addr));
    REQUIRE(server.is_connected());
    REQUIRE(client.is_connected());
}

TEST_CASE("LoopbackTransport client sends to server", "[transport][loopback]") {
    auto hub = std::make_shared<LoopbackHub>();
    LoopbackTransport server(hub);
    LoopbackTransport client(hub);

    Address server_addr("127.0.0.1", 20001);
    server.bind(server_addr);
    client.connect(server_addr);

    const char* msg = "loopback hello";
    REQUIRE(
        client.send(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered));

    server.poll();

    uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(server.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == std::strlen(msg) + 1);
    REQUIRE(std::strcmp(reinterpret_cast<char*>(buffer), "loopback hello") == 0);
    REQUIRE(sender.is_valid());
}

TEST_CASE("LoopbackTransport server broadcasts to multiple clients", "[transport][loopback]") {
    auto hub = std::make_shared<LoopbackHub>();
    LoopbackTransport server(hub);
    LoopbackTransport client1(hub);
    LoopbackTransport client2(hub);

    Address server_addr("127.0.0.1", 20002);
    server.bind(server_addr);
    client1.connect(server_addr);
    client2.connect(server_addr);

    const char* msg = "broadcast";
    REQUIRE(
        server.send(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered));

    client1.poll();
    client2.poll();

    uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;

    REQUIRE(client1.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == std::strlen(msg) + 1);
    REQUIRE(std::strcmp(reinterpret_cast<char*>(buffer), "broadcast") == 0);

    REQUIRE(client2.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(len == std::strlen(msg) + 1);
    REQUIRE(std::strcmp(reinterpret_cast<char*>(buffer), "broadcast") == 0);
}

TEST_CASE("LoopbackTransport server send_to targets specific client", "[transport][loopback]") {
    auto hub = std::make_shared<LoopbackHub>();
    LoopbackTransport server(hub);
    LoopbackTransport client1(hub);
    LoopbackTransport client2(hub);

    Address server_addr("127.0.0.1", 20003);
    server.bind(server_addr);
    client1.connect(server_addr);
    client2.connect(server_addr);

    // Server needs to know client addresses to target them.
    // In real usage NetworkManager maps ClientId → Address.
    // Here we poll the server once to capture client addresses from connect handshake.
    // For simplicity, we use the fact that client addresses are auto-assigned.
    // Instead, we'll just have the clients send something first so the server sees their addresses.
    client1.send(reinterpret_cast<const uint8_t*>("x"), 1, PacketReliability::ReliableOrdered);
    client2.send(reinterpret_cast<const uint8_t*>("x"), 1, PacketReliability::ReliableOrdered);

    server.poll();

    Address c1_addr;
    Address c2_addr;
    {
        uint8_t buf[256];
        std::size_t len = 0;
        REQUIRE(server.pop_receive(buf, sizeof(buf), len, c1_addr));
        REQUIRE(server.pop_receive(buf, sizeof(buf), len, c2_addr));
    }

    // Now send targeted messages
    server.send_to(c1_addr, reinterpret_cast<const uint8_t*>("to c1"), 6, PacketReliability::ReliableOrdered);
    server.send_to(c2_addr, reinterpret_cast<const uint8_t*>("to c2"), 6, PacketReliability::ReliableOrdered);

    client1.poll();
    client2.poll();

    uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;

    REQUIRE(client1.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(std::strcmp(reinterpret_cast<char*>(buffer), "to c1") == 0);

    REQUIRE(client2.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(std::strcmp(reinterpret_cast<char*>(buffer), "to c2") == 0);
}

TEST_CASE("LoopbackTransport supports artificial latency", "[transport][loopback]") {
    auto hub = std::make_shared<LoopbackHub>();
    LoopbackTransport server(hub);
    LoopbackTransport client(hub);

    Address server_addr("127.0.0.1", 20004);
    server.bind(server_addr);
    client.connect(server_addr);

    client.set_latency(0.05f); // 50ms one-way

    const char* msg = "delayed";
    client.send(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered);

    // Immediately poll — message should not arrive yet
    server.poll();
    uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(!server.pop_receive(buffer, sizeof(buffer), len, sender));

    // Wait for latency, then client must poll to flush its delayed queue
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    client.poll();
    server.poll();

    REQUIRE(server.pop_receive(buffer, sizeof(buffer), len, sender));
    REQUIRE(std::strcmp(reinterpret_cast<char*>(buffer), "delayed") == 0);
}

TEST_CASE("LoopbackTransport supports artificial packet loss", "[transport][loopback]") {
    auto hub = std::make_shared<LoopbackHub>();
    LoopbackTransport server(hub);
    LoopbackTransport client(hub);

    Address server_addr("127.0.0.1", 20005);
    server.bind(server_addr);
    client.connect(server_addr);

    client.set_packet_loss(1.0f); // 100% loss

    const char* msg = "lost";
    REQUIRE(
        client.send(reinterpret_cast<const uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered));

    server.poll();

    uint8_t buffer[256] = {};
    std::size_t len = 0;
    Address sender;
    REQUIRE(!server.pop_receive(buffer, sizeof(buffer), len, sender));
}

TEST_CASE("LoopbackTransport RTT and packet_loss queries", "[transport][loopback]") {
    LoopbackTransport t;
    t.set_latency(0.025f);
    t.set_packet_loss(0.1f);

    REQUIRE(t.rtt() == 0.05f);
    REQUIRE(t.packet_loss() == 0.1f);

    Address dummy("127.0.0.1", 1);
    REQUIRE(t.get_connection_rtt(dummy) == 0.05f);
    REQUIRE(t.get_connection_packet_loss(dummy) == 0.1f);
}

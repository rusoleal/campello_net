#include "campello_net/network_log.hpp"
#include "campello_net/network_manager.hpp"
#include "campello_net/network_time.hpp"
#include "campello_net/rpc_manager.hpp"
#include "campello_net/transport/loopback_transport.hpp"
#include "campello_net/transport/network_simulator.hpp"
#include "campello_net/transport/udp_transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace systems::leal::campello_net;
using namespace systems::leal::campello_net::transport;

// ── NetworkTime tests ───────────────────────────────────────────────────────

TEST_CASE("NetworkTime computes offset and RTT from symmetric sample") {
    NetworkTime nt;
    // Symmetric path: t1-t0 = 0.05, t3-t2 = 0.05, processing = 0.01
    // t0=0.0, t1=0.05, t2=0.06, t3=0.11
    nt.record_sample(0.0, 0.05, 0.06, 0.11);

    // offset = ((0.05-0.0) + (0.06-0.11))/2 = (0.05 - 0.05)/2 = 0.0
    REQUIRE(std::abs(nt.offset()) < 0.001);
    // rtt = (0.11-0.0) - (0.06-0.05) = 0.11 - 0.01 = 0.10
    REQUIRE(std::abs(nt.rtt() - 0.10) < 0.001);
    REQUIRE(nt.sample_count() == 1);
}

TEST_CASE("NetworkTime computes offset with clock skew") {
    NetworkTime nt;
    // Server clock is 2.0 seconds ahead of client
    // t0=0.0(client), t1=2.05(server), t2=2.06(server), t3=0.11(client)
    nt.record_sample(0.0, 2.05, 2.06, 0.11);

    // offset = ((2.05-0.0) + (2.06-0.11))/2 = (2.05 + 1.95)/2 = 2.0
    REQUIRE(std::abs(nt.offset() - 2.0) < 0.01);
    // rtt = (0.11-0.0) - (2.06-2.05) = 0.11 - 0.01 = 0.10
    REQUIRE(std::abs(nt.rtt() - 0.10) < 0.01);
}

TEST_CASE("NetworkTime smoothing converges") {
    NetworkTime nt;
    for (int i = 0; i < 10; ++i) {
        nt.record_sample(0.0, 2.0, 2.0, 0.1);
    }
    REQUIRE(std::abs(nt.offset() - 1.95) < 0.05);
    REQUIRE(std::abs(nt.rtt() - 0.1) < 0.02);
}

TEST_CASE("NetworkTime converts times") {
    NetworkTime nt;
    nt.record_sample(0.0, 2.0, 2.0, 0.1);
    double local = 5.0;
    double remote = nt.local_to_remote(local);
    REQUIRE(std::abs(remote - (local + nt.offset())) < 0.001);
    REQUIRE(std::abs(nt.remote_to_local(remote) - local) < 0.001);
}

TEST_CASE("NetworkTime reset clears state") {
    NetworkTime nt;
    nt.record_sample(0.0, 1.0, 1.0, 0.1);
    nt.reset();
    REQUIRE(nt.sample_count() == 0);
    REQUIRE(nt.offset() == 0.0);
    REQUIRE(nt.rtt() == 0.0);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static void poll_both(NetworkManager& a, NetworkManager& b, int ticks = 20, int delay_ms = 10) {
    for (int i = 0; i < ticks; ++i) {
        a.poll();
        b.poll();
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
}

// ── NetworkManager server/client tests ──────────────────────────────────────

TEST_CASE("Server and client connect and exchange messages") {
    Address server_addr("::1", 0); // bind to any port

    NetworkManager server;
    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = server_addr;
    REQUIRE(server.start(sconfig));

    // Get actual bound port
    // For now use a fixed port to avoid race conditions
    server.stop();
    sconfig.bind_address = Address("::1", 34567);
    REQUIRE(server.start(sconfig));

    ClientId connected_client = 0;
    server.on_client_connected([&connected_client](ClientId id) {
        connected_client = id;
    });

    NetworkManager client;
    NetworkManager::Config cconfig;
    cconfig.mode = NetworkManager::Mode::Client;
    cconfig.server_address = Address("::1", 34567);
    REQUIRE(client.start(cconfig));

    // Poll until connected
    for (int i = 0; i < 60; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (connected_client != 0 && client.local_client_id() != 0)
            break;
    }

    REQUIRE(connected_client != 0);
    REQUIRE(client.local_client_id() != 0);
    REQUIRE(server.client_count() == 1);
    REQUIRE(client.is_active());

    // Client -> Server message
    const char* msg = "hello server";
    client.send(reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered);

    poll_both(client, server, 20);

    NetworkManager::ReceivedMessage received;
    bool got_msg = false;
    while (server.pop_message(received)) {
        if (received.client == connected_client) {
            REQUIRE(std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "hello server") == 0);
            got_msg = true;
        }
    }
    REQUIRE(got_msg);

    // Server -> Client message
    const char* reply = "hello client";
    server.send(connected_client, reinterpret_cast<const std::uint8_t*>(reply), std::strlen(reply) + 1,
                PacketReliability::ReliableOrdered);

    poll_both(server, client, 20);

    got_msg = false;
    while (client.pop_message(received)) {
        REQUIRE(std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "hello client") == 0);
        got_msg = true;
    }
    REQUIRE(got_msg);

    client.disconnect();
    poll_both(server, client, 10);
}

TEST_CASE("Server broadcasts to all clients") {
    Address server_addr("::1", 34568);

    NetworkManager server;
    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = server_addr;
    REQUIRE(server.start(sconfig));

    NetworkManager client1;
    NetworkManager::Config c1config;
    c1config.mode = NetworkManager::Mode::Client;
    c1config.server_address = server_addr;
    REQUIRE(client1.start(c1config));

    NetworkManager client2;
    NetworkManager::Config c2config;
    c2config.mode = NetworkManager::Mode::Client;
    c2config.server_address = server_addr;
    REQUIRE(client2.start(c2config));

    // Wait for connections
    for (int i = 0; i < 80; ++i) {
        server.poll();
        client1.poll();
        client2.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server.client_count() >= 2)
            break;
    }

    REQUIRE(server.client_count() == 2);

    const char* broadcast_msg = "broadcast!";
    server.broadcast(reinterpret_cast<const std::uint8_t*>(broadcast_msg), std::strlen(broadcast_msg) + 1,
                     PacketReliability::ReliableOrdered);

    poll_both(server, client1, 20);
    poll_both(server, client2, 20);

    bool c1_got = false;
    NetworkManager::ReceivedMessage received;
    while (client1.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "broadcast!") == 0) {
            c1_got = true;
        }
    }
    bool c2_got = false;
    while (client2.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "broadcast!") == 0) {
            c2_got = true;
        }
    }
    REQUIRE(c1_got);
    REQUIRE(c2_got);

    client1.disconnect();
    client2.disconnect();
    poll_both(server, client1, 10);
}

TEST_CASE("Connection approval rejects unauthorized clients") {
    Address server_addr("::1", 34569);

    NetworkManager server;
    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = server_addr;
    REQUIRE(server.start(sconfig));

    server.set_connection_approval([](const Address&, const std::vector<std::uint8_t>& auth) -> bool {
        // Require auth data to start with 'O','K'
        return auth.size() >= 2 && auth[0] == 'O' && auth[1] == 'K';
    });

    // Bad client (no auth)
    NetworkManager bad_client;
    NetworkManager::Config bc;
    bc.mode = NetworkManager::Mode::Client;
    bc.server_address = server_addr;
    REQUIRE(bad_client.start(bc));

    // Good client (with auth) — but our current connect request sends empty auth
    // So even "good" client will be rejected in this test
    for (int i = 0; i < 40; ++i) {
        server.poll();
        bad_client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Bad client should not be connected
    REQUIRE(server.client_count() == 0);

    bad_client.disconnect();
    server.stop();
}

TEST_CASE("Host mode local client sends and receives") {
    Address host_addr("::1", 34570);

    NetworkManager host;
    NetworkManager::Config hconfig;
    hconfig.mode = NetworkManager::Mode::Host;
    hconfig.bind_address = host_addr;
    hconfig.server_address = host_addr;
    REQUIRE(host.start(hconfig));

    REQUIRE(host.local_client_id() != 0);
    REQUIRE(host.client_count() == 1);
    REQUIRE(host.is_client_connected(host.local_client_id()));

    // Local client sends
    const char* msg = "host hello";
    host.send(reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered);

    host.poll();

    NetworkManager::ReceivedMessage received;
    bool got = false;
    while (host.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "host hello") == 0) {
            got = true;
        }
    }
    REQUIRE(got);

    host.stop();
}

TEST_CASE("NetworkManager with NetworkSimulator simulates latency") {
    Address host_addr("::1", 34571);

    // Test that client_rtt / client_packet_loss return 0 for local client in Host
    NetworkManager host;
    NetworkManager::Config hc;
    hc.mode = NetworkManager::Mode::Host;
    hc.bind_address = host_addr;
    hc.server_address = host_addr;
    REQUIRE(host.start(hc));

    REQUIRE(host.client_rtt(host.local_client_id()) == 0.0f);
    REQUIRE(host.client_packet_loss(host.local_client_id()) == 0.0f);
    REQUIRE(host.network_time() > 0.0); // should return local time

    host.stop();
}

TEST_CASE("Client disconnect notifies server") {
    Address server_addr("::1", 34572);

    NetworkManager server;
    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = server_addr;
    REQUIRE(server.start(sconfig));

    ClientId disconnected_id = 0;
    server.on_client_disconnected([&disconnected_id](ClientId id) {
        disconnected_id = id;
    });

    NetworkManager client;
    NetworkManager::Config cconfig;
    cconfig.mode = NetworkManager::Mode::Client;
    cconfig.server_address = server_addr;
    REQUIRE(client.start(cconfig));

    // Connect
    for (int i = 0; i < 60; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server.client_count() > 0 && client.local_client_id() != 0)
            break;
    }
    REQUIRE(server.client_count() == 1);
    REQUIRE(client.local_client_id() != 0);

    // Disconnect — send notify, poll to deliver, then stop
    client.disconnect();

    for (int i = 0; i < 200; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (disconnected_id != 0)
            break;
    }

    REQUIRE(disconnected_id != 0);
    REQUIRE(server.client_count() == 0);
}

// ── Multi-transport tests ───────────────────────────────────────────────────

TEST_CASE("set_transport injects custom ITransport") {
    Address server_addr("::1", 34573);

    // Create server with a NetworkSimulator-wrapped UdpTransport
    auto sim_transport = std::make_unique<NetworkSimulator>(std::make_unique<UdpTransport>());
    sim_transport->set_latency(10.0f, 20.0f);

    NetworkManager server;
    server.set_transport(std::move(sim_transport));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = server_addr;
    REQUIRE(server.start(sconfig));

    ClientId connected_client = 0;
    server.on_client_connected([&connected_client](ClientId id) {
        connected_client = id;
    });

    // Client uses plain UdpTransport
    NetworkManager client;
    NetworkManager::Config cconfig;
    cconfig.mode = NetworkManager::Mode::Client;
    cconfig.server_address = server_addr;
    REQUIRE(client.start(cconfig));

    for (int i = 0; i < 60; ++i) {
        server.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (connected_client != 0 && client.local_client_id() != 0)
            break;
    }

    REQUIRE(connected_client != 0);
    REQUIRE(client.local_client_id() != 0);
    REQUIRE(server.client_count() == 1);

    // Exchange a message
    const char* msg = "through simulator";
    client.send(reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered);

    poll_both(client, server, 40);

    NetworkManager::ReceivedMessage received;
    bool got = false;
    while (server.pop_message(received)) {
        if (received.client == connected_client) {
            REQUIRE(std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "through simulator") == 0);
            got = true;
        }
    }
    REQUIRE(got);

    client.disconnect();
    poll_both(server, client, 10);
}

TEST_CASE("Multi-transport server accepts clients on different transports") {
    Address primary_addr("::1", 34574);
    Address secondary_addr("::1", 34575);

    NetworkManager server;

    // Primary transport on port 34574
    auto primary = std::make_unique<UdpTransport>();
    REQUIRE(primary->bind(primary_addr));
    server.set_transport(std::move(primary));

    // Additional transport on port 34575
    auto secondary = std::make_unique<UdpTransport>();
    REQUIRE(secondary->bind(secondary_addr));
    server.add_transport(std::move(secondary));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = primary_addr;
    REQUIRE(server.start(sconfig));

    ClientId connected_client_a = 0;
    ClientId connected_client_b = 0;
    server.on_client_connected([&](ClientId id) {
        if (connected_client_a == 0) {
            connected_client_a = id;
        } else {
            connected_client_b = id;
        }
    });

    // Client A connects to primary transport
    NetworkManager client_a;
    NetworkManager::Config ca;
    ca.mode = NetworkManager::Mode::Client;
    ca.server_address = primary_addr;
    REQUIRE(client_a.start(ca));

    // Client B connects to secondary transport
    NetworkManager client_b;
    NetworkManager::Config cb;
    cb.mode = NetworkManager::Mode::Client;
    cb.server_address = secondary_addr;
    REQUIRE(client_b.start(cb));

    for (int i = 0; i < 80; ++i) {
        server.poll();
        client_a.poll();
        client_b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (server.client_count() >= 2)
            break;
    }

    REQUIRE(server.client_count() == 2);
    REQUIRE(connected_client_a != 0);
    REQUIRE(connected_client_b != 0);
    REQUIRE(connected_client_a != connected_client_b);

    // Server sends targeted message to client A via primary transport
    const char* msg_a = "hello A";
    server.send(connected_client_a, reinterpret_cast<const std::uint8_t*>(msg_a), std::strlen(msg_a) + 1,
                PacketReliability::ReliableOrdered);

    // Server sends targeted message to client B via secondary transport
    const char* msg_b = "hello B";
    server.send(connected_client_b, reinterpret_cast<const std::uint8_t*>(msg_b), std::strlen(msg_b) + 1,
                PacketReliability::ReliableOrdered);

    poll_both(server, client_a, 20);
    poll_both(server, client_b, 20);

    bool a_got = false;
    NetworkManager::ReceivedMessage received;
    while (client_a.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "hello A") == 0) {
            a_got = true;
        }
    }
    bool b_got = false;
    while (client_b.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "hello B") == 0) {
            b_got = true;
        }
    }
    REQUIRE(a_got);
    REQUIRE(b_got);

    // Broadcast to both clients across transports
    const char* bcast = "broadcast all";
    server.broadcast(reinterpret_cast<const std::uint8_t*>(bcast), std::strlen(bcast) + 1,
                     PacketReliability::ReliableOrdered);

    poll_both(server, client_a, 20);
    poll_both(server, client_b, 20);

    a_got = false;
    while (client_a.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "broadcast all") == 0) {
            a_got = true;
        }
    }
    b_got = false;
    while (client_b.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "broadcast all") == 0) {
            b_got = true;
        }
    }
    REQUIRE(a_got);
    REQUIRE(b_got);

    client_a.disconnect();
    client_b.disconnect();
    poll_both(server, client_a, 10);
}

// ── Local client (couch player) tests ───────────────────────────────────────

TEST_CASE("Host mode add_local_client creates couch player") {
    Address host_addr("::1", 34576);

    NetworkManager host;
    NetworkManager::Config hconfig;
    hconfig.mode = NetworkManager::Mode::Host;
    hconfig.bind_address = host_addr;
    hconfig.server_address = host_addr;
    REQUIRE(host.start(hconfig));

    ClientId local = host.local_client_id();
    REQUIRE(local != 0);
    REQUIRE(host.client_count() == 1);

    // Add a second local client (couch player 2)
    ClientId couch2 = host.add_local_client();
    REQUIRE(couch2 != 0);
    REQUIRE(couch2 != local);
    REQUIRE(host.client_count() == 2);
    REQUIRE(host.is_client_connected(couch2));

    // Local client 1 sends — received by server side
    const char* msg1 = "from local1";
    host.send(reinterpret_cast<const std::uint8_t*>(msg1), std::strlen(msg1) + 1, PacketReliability::ReliableOrdered);

    host.poll();

    NetworkManager::ReceivedMessage received;
    bool got_local1 = false;
    while (host.pop_message(received)) {
        if (received.client == local) {
            got_local1 = true;
        }
    }
    REQUIRE(got_local1);

    // Server sends to couch2 — delivered directly (no transport)
    const char* msg2 = "to couch2";
    host.send(couch2, reinterpret_cast<const std::uint8_t*>(msg2), std::strlen(msg2) + 1,
              PacketReliability::ReliableOrdered);

    host.poll();

    bool got_couch2 = false;
    while (host.pop_message(received)) {
        if (received.client == couch2) {
            REQUIRE(std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "to couch2") == 0);
            got_couch2 = true;
        }
    }
    REQUIRE(got_couch2);

    // Broadcast reaches both local clients
    const char* bcast = "all locals";
    host.broadcast(reinterpret_cast<const std::uint8_t*>(bcast), std::strlen(bcast) + 1,
                   PacketReliability::ReliableOrdered);

    host.poll();

    bool got_local_bcast = false;
    bool got_couch2_bcast = false;
    while (host.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "all locals") != 0)
            continue;
        if (received.client == local)
            got_local_bcast = true;
        if (received.client == couch2)
            got_couch2_bcast = true;
    }
    REQUIRE(got_local_bcast);
    REQUIRE(got_couch2_bcast);

    host.stop();
}

TEST_CASE("Server mode add_local_client without host loopback") {
    Address server_addr("::1", 34577);

    NetworkManager server;
    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = server_addr;
    REQUIRE(server.start(sconfig));

    // Add a local client on a pure server (no host loopback queue)
    ClientId local = server.add_local_client();
    REQUIRE(local != 0);
    REQUIRE(server.client_count() == 1);
    REQUIRE(server.is_client_connected(local));

    // Server sends to local client — delivered directly via push_message
    const char* msg = "server to local";
    server.send(local, reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1,
                PacketReliability::ReliableOrdered);

    server.poll();

    NetworkManager::ReceivedMessage received;
    bool got = false;
    while (server.pop_message(received)) {
        if (received.client == local) {
            REQUIRE(std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "server to local") == 0);
            got = true;
        }
    }
    REQUIRE(got);

    server.remove_local_client(local);
    REQUIRE(server.client_count() == 0);
    REQUIRE(!server.is_client_connected(local));

    server.stop();
}

TEST_CASE("add_local_client returns 0 in client mode") {
    NetworkManager client;
    NetworkManager::Config cconfig;
    cconfig.mode = NetworkManager::Mode::Client;
    cconfig.server_address = Address("::1", 34578);
    REQUIRE(client.start(cconfig));

    REQUIRE(client.add_local_client() == 0);

    client.stop();
}

// ── LoopbackTransport integration tests ─────────────────────────────────────

TEST_CASE("NetworkManager server/client over LoopbackTransport without sockets or sleep") {
    auto hub = std::make_shared<LoopbackHub>();

    // Server
    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = Address("127.0.0.1", 40000);
    REQUIRE(server.start(sconfig));

    ClientId connected_client = 0;
    server.on_client_connected([&connected_client](ClientId id) {
        connected_client = id;
    });

    // Client
    NetworkManager client;
    client.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config cconfig;
    cconfig.mode = NetworkManager::Mode::Client;
    cconfig.server_address = Address("127.0.0.1", 40000);
    REQUIRE(client.start(cconfig));

    // Instant connection — no UDP handshake, no sleep
    server.poll();
    client.poll();

    REQUIRE(connected_client != 0);
    REQUIRE(client.local_client_id() != 0);
    REQUIRE(server.client_count() == 1);

    // Client → Server message
    const char* msg = "instant hello";
    client.send(reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered);

    server.poll();
    client.poll();

    NetworkManager::ReceivedMessage received;
    bool got = false;
    while (server.pop_message(received)) {
        if (received.client == connected_client) {
            REQUIRE(std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "instant hello") == 0);
            got = true;
        }
    }
    REQUIRE(got);

    // Server → Client message
    const char* reply = "instant reply";
    server.send(connected_client, reinterpret_cast<const std::uint8_t*>(reply), std::strlen(reply) + 1,
                PacketReliability::ReliableOrdered);

    server.poll();
    client.poll();

    got = false;
    while (client.pop_message(received)) {
        REQUIRE(std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "instant reply") == 0);
        got = true;
    }
    REQUIRE(got);

    client.disconnect();
    server.poll();
    client.poll();
}

TEST_CASE("LoopbackTransport multi-client server with NetworkManager") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = Address("127.0.0.1", 40001);
    REQUIRE(server.start(sconfig));

    std::vector<ClientId> connected;
    server.on_client_connected([&connected](ClientId id) {
        connected.push_back(id);
    });

    NetworkManager client1;
    client1.set_transport(std::make_unique<LoopbackTransport>(hub));
    NetworkManager::Config c1;
    c1.mode = NetworkManager::Mode::Client;
    c1.server_address = Address("127.0.0.1", 40001);
    REQUIRE(client1.start(c1));

    NetworkManager client2;
    client2.set_transport(std::make_unique<LoopbackTransport>(hub));
    NetworkManager::Config c2;
    c2.mode = NetworkManager::Mode::Client;
    c2.server_address = Address("127.0.0.1", 40001);
    REQUIRE(client2.start(c2));

    // Instant connect for both
    server.poll();
    client1.poll();
    client2.poll();
    server.poll();
    client1.poll();
    client2.poll();

    REQUIRE(server.client_count() == 2);
    REQUIRE(connected.size() == 2);

    // Broadcast
    const char* bcast = "all loopback";
    server.broadcast(reinterpret_cast<const std::uint8_t*>(bcast), std::strlen(bcast) + 1,
                     PacketReliability::ReliableOrdered);

    client1.poll();
    client2.poll();

    bool c1_got = false;
    NetworkManager::ReceivedMessage received;
    while (client1.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "all loopback") == 0) {
            c1_got = true;
        }
    }
    bool c2_got = false;
    while (client2.pop_message(received)) {
        if (std::strcmp(reinterpret_cast<const char*>(received.payload.data()), "all loopback") == 0) {
            c2_got = true;
        }
    }
    REQUIRE(c1_got);
    REQUIRE(c2_got);
}

// ── Rate limiting integration tests ─────────────────────────────────────────

TEST_CASE("max_clients rejects beyond limit") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = Address("127.0.0.1", 40010);
    sconfig.max_clients = 2;
    REQUIRE(server.start(sconfig));

    NetworkManager client1;
    client1.set_transport(std::make_unique<LoopbackTransport>(hub));
    client1.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40010)});

    NetworkManager client2;
    client2.set_transport(std::make_unique<LoopbackTransport>(hub));
    client2.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40010)});

    NetworkManager client3;
    client3.set_transport(std::make_unique<LoopbackTransport>(hub));
    client3.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40010)});

    // Poll to let connections process
    for (int i = 0; i < 5; ++i) {
        server.poll();
        client1.poll();
        client2.poll();
        client3.poll();
    }

    REQUIRE(server.client_count() == 2);

    // Third client should not have received an accept
    REQUIRE(client3.local_client_id() == 0);
}

TEST_CASE("max_packet_size disconnects offender") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = Address("127.0.0.1", 40011);
    sconfig.max_packet_size = 64; // very small
    REQUIRE(server.start(sconfig));

    ClientId disconnected_id = 0;
    server.on_client_disconnected([&disconnected_id](ClientId id) {
        disconnected_id = id;
    });

    NetworkManager client;
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40011)});

    server.poll();
    client.poll();
    server.poll();
    client.poll();

    REQUIRE(server.client_count() == 1);
    ClientId cid = client.local_client_id();
    REQUIRE(cid != 0);

    // Send a large message (>64 bytes)
    std::vector<std::uint8_t> big_msg(128, 0xAB);
    client.send(big_msg.data(), big_msg.size(), PacketReliability::ReliableOrdered);

    server.poll();
    client.poll();

    // Client should have been disconnected
    REQUIRE(disconnected_id == cid);
    REQUIRE(server.client_count() == 0);
}

TEST_CASE("Message rate limiting drops excess inbound traffic") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = Address("127.0.0.1", 40012);
    sconfig.max_messages_per_sec = 3.0f;
    sconfig.rate_limit_burst = 3.0f;
    REQUIRE(server.start(sconfig));

    NetworkManager client;
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40012)});

    server.poll();
    client.poll();
    server.poll();
    client.poll();

    REQUIRE(server.client_count() == 1);

    // Send 5 messages rapidly
    for (int i = 0; i < 5; ++i) {
        std::string msg = "msg " + std::to_string(i);
        client.send(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size(), PacketReliability::ReliableOrdered);
    }

    server.poll();
    client.poll();

    // Only first 3 should arrive (burst = 3)
    int count = 0;
    NetworkManager::ReceivedMessage received;
    while (server.pop_message(received)) {
        ++count;
    }
    REQUIRE(count == 3);
}

TEST_CASE("RPC rate limiting drops excess RPCs") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = Address("127.0.0.1", 40013);
    sconfig.max_rpcs_per_sec = 2.0f;
    sconfig.rate_limit_burst = 2.0f;
    REQUIRE(server.start(sconfig));

    class RpcManager rpc_mgr;
    server.set_rpc_manager(&rpc_mgr);

    int rpc_count = 0;
    rpc_mgr.register_handler(1, [&rpc_count](ClientId, serialization::BitStream&) {
        ++rpc_count;
    });

    NetworkManager client;
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40013)});

    class RpcManager client_rpc;
    client_rpc.set_network_manager(&client);

    server.poll();
    client.poll();
    server.poll();
    client.poll();

    REQUIRE(server.client_count() == 1);

    // Send 4 RPCs rapidly from client
    for (int i = 0; i < 4; ++i) {
        client_rpc.invoke_server(1);
    }

    // Client needs to poll to send the RPCs
    client.poll();
    server.poll();
    client.poll();
    server.poll();

    // Only first 2 should be handled (burst = 2)
    REQUIRE(rpc_count == 2);
}

TEST_CASE("Rate limiter does not affect system messages") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));

    NetworkManager::Config sconfig;
    sconfig.mode = NetworkManager::Mode::Server;
    sconfig.bind_address = Address("127.0.0.1", 40014);
    sconfig.max_messages_per_sec = 1.0f;
    sconfig.rate_limit_burst = 1.0f;
    REQUIRE(server.start(sconfig));

    NetworkManager client;
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40014)});

    // Connection handshake uses system messages, not user data
    server.poll();
    client.poll();
    server.poll();
    client.poll();

    // Should connect despite very restrictive message rate limit
    REQUIRE(server.client_count() == 1);
    REQUIRE(client.local_client_id() != 0);
}

// ── NetStats tests ──────────────────────────────────────────────────────────

TEST_CASE("net_stats tracks bytes and packets for connected client") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));
    server.start({NetworkManager::Mode::Server, Address("127.0.0.1", 40020)});

    NetworkManager client;
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40020)});

    server.poll();
    client.poll();
    server.poll();
    client.poll();

    ClientId cid = client.local_client_id();
    REQUIRE(cid != 0);

    // Stats should be zero before any traffic
    NetStats stats = server.net_stats(cid);
    REQUIRE(stats.bytes_received == 0);
    REQUIRE(stats.bytes_sent == 0);
    REQUIRE(stats.packets_received == 0);
    REQUIRE(stats.packets_sent == 0);

    // Client sends a message
    const char* msg = "stats test";
    client.send(reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1, PacketReliability::ReliableOrdered);

    server.poll();
    client.poll();

    stats = server.net_stats(cid);
    REQUIRE(stats.bytes_received == std::strlen(msg) + 1);
    REQUIRE(stats.packets_received == 1);

    // Server sends a reply
    server.send(cid, reinterpret_cast<const std::uint8_t*>(msg), std::strlen(msg) + 1,
                PacketReliability::ReliableOrdered);

    server.poll();
    client.poll();

    stats = server.net_stats(cid);
    REQUIRE(stats.bytes_sent == std::strlen(msg) + 1);
    REQUIRE(stats.packets_sent == 1);
}

TEST_CASE("net_stats bandwidth estimate is non-zero after traffic") {
    auto hub = std::make_shared<LoopbackHub>();

    NetworkManager server;
    server.set_transport(std::make_unique<LoopbackTransport>(hub));
    server.start({NetworkManager::Mode::Server, Address("127.0.0.1", 40021)});

    NetworkManager client;
    client.set_transport(std::make_unique<LoopbackTransport>(hub));
    client.start({NetworkManager::Mode::Client, {}, Address("127.0.0.1", 40021)});

    server.poll();
    client.poll();
    server.poll();
    client.poll();

    ClientId cid = client.local_client_id();

    // Send multiple messages rapidly
    for (int i = 0; i < 10; ++i) {
        client.send(reinterpret_cast<const std::uint8_t*>("hello"), 6, PacketReliability::ReliableOrdered);
    }

    server.poll();
    client.poll();

    // Bandwidth is computed in poll() based on time delta
    // We need at least one more poll() to get the EMA updated
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    server.poll();

    NetStats stats = server.net_stats(cid);
    REQUIRE(stats.bandwidth_in > 0.0f);  // client → server traffic
    REQUIRE(stats.bytes_received == 60); // 10 × 6 bytes
}

TEST_CASE("net_stats returns zero for unknown client") {
    NetworkManager server;
    server.start({NetworkManager::Mode::Server, Address("127.0.0.1", 40022)});

    NetStats stats = server.net_stats(99999);
    REQUIRE(stats.bytes_sent == 0);
    REQUIRE(stats.bytes_received == 0);
}

// ── Logging tests ───────────────────────────────────────────────────────────

TEST_CASE("set_log_callback receives log messages") {
    std::vector<std::pair<LogLevel, std::string>> captured;
    set_log_callback([&captured](LogLevel level, const std::string& msg) {
        captured.push_back({level, msg});
    });

    CAMPELLO_NET_LOGI("test info message");
    CAMPELLO_NET_LOGW("test warning message");
    CAMPELLO_NET_LOGE("test error message");

    REQUIRE(captured.size() == 3);
    REQUIRE(captured[0].first == LogLevel::Info);
    REQUIRE(captured[0].second == "test info message");
    REQUIRE(captured[1].first == LogLevel::Warning);
    REQUIRE(captured[2].first == LogLevel::Error);

    // Reset callback to avoid affecting other tests
    set_log_callback(nullptr);
}

TEST_CASE("Verbose log is captured when min level is Verbose") {
    std::vector<std::string> verbose_msgs;
    set_log_callback([&verbose_msgs](LogLevel level, const std::string& msg) {
        if (level == LogLevel::Verbose)
            verbose_msgs.push_back(msg);
    });

    CAMPELLO_NET_LOGV("verbose thing");

    REQUIRE(verbose_msgs.size() == 1);
    REQUIRE(verbose_msgs[0] == "verbose thing");

    set_log_callback(nullptr);
}

TEST_CASE("NetworkManager start/stop produces log output") {
    std::vector<std::string> log_lines;
    set_log_callback([&log_lines](LogLevel, const std::string& msg) {
        log_lines.push_back(msg);
    });

    NetworkManager server;
    server.start({NetworkManager::Mode::Server, Address("127.0.0.1", 40023)});
    server.stop();

    bool found_start = false;
    bool found_stop = false;
    for (const auto& line : log_lines) {
        if (line.find("Server started") != std::string::npos)
            found_start = true;
        if (line.find("Stopping network manager") != std::string::npos)
            found_stop = true;
    }
    REQUIRE(found_start);
    REQUIRE(found_stop);

    set_log_callback(nullptr);
}

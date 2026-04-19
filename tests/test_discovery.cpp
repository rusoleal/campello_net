#include "campello_net/discovery/lan_discovery.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace systems::leal::campello_net;

TEST_CASE("LanDiscovery beacon format round-trip") {
    LanDiscovery advertiser;
    LanDiscovery listener;

    std::vector<LanDiscovery::Beacon> received;
    listener.on_beacon_received([&received](const LanDiscovery::Beacon& b) {
        received.push_back(b);
    });

    constexpr std::uint16_t DISCO_PORT = 34600;
    constexpr std::uint16_t GAME_PORT = 34601;

    REQUIRE(advertiser.start_advertising(DISCO_PORT, GAME_PORT, "Test Server", 16, 0.05f));
    REQUIRE(listener.start_listening(DISCO_PORT));

    advertiser.set_current_players(4);

    // Poll both sides for a short while
    for (int i = 0; i < 60; ++i) {
        advertiser.poll();
        listener.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!received.empty())
            break;
    }

    REQUIRE(!received.empty());

    const auto& b = received.front();
    REQUIRE(b.server_name == "Test Server");
    REQUIRE(b.max_players == 16);
    REQUIRE(b.current_players == 4);
    REQUIRE(b.server_address.port() == GAME_PORT);

    advertiser.stop_advertising();
    listener.stop_listening();
}

TEST_CASE("LanDiscovery ignores malformed beacons") {
    LanDiscovery listener;
    std::vector<LanDiscovery::Beacon> received;
    listener.on_beacon_received([&received](const LanDiscovery::Beacon& b) {
        received.push_back(b);
    });

    constexpr std::uint16_t DISCO_PORT = 34602;
    REQUIRE(listener.start_listening(DISCO_PORT));

    // Send garbage UDP packet from a raw socket
    int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    REQUIRE(sock >= 0);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DISCO_PORT);
    dest.sin_addr.s_addr = INADDR_LOOPBACK;

    std::uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ::sendto(sock, reinterpret_cast<const char*>(garbage), sizeof(garbage), 0, reinterpret_cast<sockaddr*>(&dest),
             sizeof(dest));

#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif

    // Poll listener
    for (int i = 0; i < 20; ++i) {
        listener.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE(received.empty());
    listener.stop_listening();
}

TEST_CASE("LanDiscovery advertiser can update player count") {
    LanDiscovery advertiser;
    LanDiscovery listener;

    std::vector<LanDiscovery::Beacon> received;
    listener.on_beacon_received([&received](const LanDiscovery::Beacon& b) {
        received.push_back(b);
    });

    constexpr std::uint16_t DISCO_PORT = 34603;
    REQUIRE(advertiser.start_advertising(DISCO_PORT, 34604, "Counter", 8, 0.05f));
    REQUIRE(listener.start_listening(DISCO_PORT));

    // First beacon: 0 players
    for (int i = 0; i < 40; ++i) {
        advertiser.poll();
        listener.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!received.empty())
            break;
    }
    REQUIRE(!received.empty());
    REQUIRE(received.back().current_players == 0);

    // Update count
    advertiser.set_current_players(3);
    received.clear();

    for (int i = 0; i < 40; ++i) {
        advertiser.poll();
        listener.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!received.empty())
            break;
    }
    REQUIRE(!received.empty());
    REQUIRE(received.back().current_players == 3);

    advertiser.stop_advertising();
    listener.stop_listening();
}

#include "campello_net/discovery/lan_discovery.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace systems::leal::campello_net {

// ════════════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════════════

namespace {

inline bool is_valid(socket_t s) noexcept {
#ifdef _WIN32
    return s != INVALID_SOCKET;
#else
    return s >= 0;
#endif
}

inline void set_nonblocking(socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    ::ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags >= 0) ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline void close_sock(socket_t& s) {
    if (!is_valid(s)) return;
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
    s = static_cast<socket_t>(-1);
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ════════════════════════════════════════════════════════════════════════════

LanDiscovery::LanDiscovery() = default;

LanDiscovery::~LanDiscovery() {
    close_socket();
}

// ════════════════════════════════════════════════════════════════════════════
// Socket management
// ════════════════════════════════════════════════════════════════════════════

bool LanDiscovery::create_socket(std::uint16_t port) {
    close_socket();

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!is_valid(socket_)) return false;

    set_nonblocking(socket_);

    int broadcast = 1;
    ::setsockopt(socket_, SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

    int reuse = 1;
    ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(socket_);
        return false;
    }
    return true;
}

void LanDiscovery::close_socket() {
    close_sock(socket_);
    advertising_ = false;
    listening_ = false;
}

// ════════════════════════════════════════════════════════════════════════════
// Advertising
// ════════════════════════════════════════════════════════════════════════════

bool LanDiscovery::start_advertising(std::uint16_t discovery_port,
                                     std::uint16_t game_port,
                                     const std::string& server_name,
                                     std::uint32_t max_players,
                                     float interval_seconds) {
    // Advertiser binds to an ephemeral port (0).  It only needs to SEND
    // broadcast packets to the discovery_port; it does not need to receive
    // on that port.  This allows advertiser + listener to coexist in the
    // same process during testing.
    if (!create_socket(0)) return false;

    advertising_ = true;
    discovery_port_ = discovery_port;
    game_port_ = game_port;
    server_name_ = server_name.substr(0, 255);
    max_players_ = max_players;
    current_players_ = 0;
    advert_interval_ = interval_seconds;
    advert_accumulator_ = 0.0f;
    return true;
}

void LanDiscovery::stop_advertising() {
    if (!listening_) {
        close_socket();
    } else {
        advertising_ = false;
    }
}

void LanDiscovery::set_current_players(std::uint32_t count) noexcept {
    current_players_ = count;
}

// ════════════════════════════════════════════════════════════════════════════
// Listening
// ════════════════════════════════════════════════════════════════════════════

bool LanDiscovery::start_listening(std::uint16_t discovery_port) {
    if (!is_valid(socket_)) {
        if (!create_socket(discovery_port)) return false;
    }
    listening_ = true;
    return true;
}

void LanDiscovery::stop_listening() {
    if (!advertising_) {
        close_socket();
    } else {
        listening_ = false;
    }
}

void LanDiscovery::on_beacon_received(BeaconCallback cb) {
    beacon_cb_ = std::move(cb);
}

// ════════════════════════════════════════════════════════════════════════════
// Polling
// ════════════════════════════════════════════════════════════════════════════

void LanDiscovery::poll() {
    auto now = Clock::now();
    float dt = std::chrono::duration<float>(now - last_poll_).count();
    last_poll_ = now;

    if (advertising_) {
        advert_accumulator_ += dt;
        while (advert_accumulator_ >= advert_interval_) {
            advert_accumulator_ -= advert_interval_;
            send_beacon();
        }
    }

    if (listening_) {
        receive_beacons();
    }
}

bool LanDiscovery::is_advertising() const noexcept {
    return advertising_;
}

bool LanDiscovery::is_listening() const noexcept {
    return listening_;
}

// ════════════════════════════════════════════════════════════════════════════
// Beacon send / receive
// ════════════════════════════════════════════════════════════════════════════

void LanDiscovery::send_beacon() {
    if (!is_valid(socket_)) return;

    std::vector<std::uint8_t> packet;
    packet.reserve(16 + server_name_.size());

    // Magic (big-endian)
    packet.push_back(static_cast<std::uint8_t>(MAGIC >> 24));
    packet.push_back(static_cast<std::uint8_t>(MAGIC >> 16));
    packet.push_back(static_cast<std::uint8_t>(MAGIC >> 8));
    packet.push_back(static_cast<std::uint8_t>(MAGIC));

    // Version
    packet.push_back(VERSION);

    // Game port (big-endian)
    packet.push_back(static_cast<std::uint8_t>(game_port_ >> 8));
    packet.push_back(static_cast<std::uint8_t>(game_port_ & 0xFF));

    // Max / current players (big-endian)
    packet.push_back(static_cast<std::uint8_t>(max_players_ >> 24));
    packet.push_back(static_cast<std::uint8_t>(max_players_ >> 16));
    packet.push_back(static_cast<std::uint8_t>(max_players_ >> 8));
    packet.push_back(static_cast<std::uint8_t>(max_players_));
    packet.push_back(static_cast<std::uint8_t>(current_players_ >> 24));
    packet.push_back(static_cast<std::uint8_t>(current_players_ >> 16));
    packet.push_back(static_cast<std::uint8_t>(current_players_ >> 8));
    packet.push_back(static_cast<std::uint8_t>(current_players_));

    // Name
    std::uint8_t name_len = static_cast<std::uint8_t>(
        std::min(server_name_.size(), std::size_t{255}));
    packet.push_back(name_len);
    packet.insert(packet.end(), server_name_.data(), server_name_.data() + name_len);

    sockaddr_in broadcast_addr{};
    broadcast_addr.sin_family = AF_INET;
    // Broadcast to the discovery port that listeners are bound to.
    broadcast_addr.sin_port = htons(discovery_port_);
    broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;

    ::sendto(socket_, reinterpret_cast<const char*>(packet.data()),
             static_cast<int>(packet.size()), 0,
             reinterpret_cast<sockaddr*>(&broadcast_addr), sizeof(broadcast_addr));

    // Also send to loopback so local testing works on hosts where
    // broadcast packets do not reach the loopback interface.
    sockaddr_in loopback_addr{};
    loopback_addr.sin_family = AF_INET;
    loopback_addr.sin_port = htons(discovery_port_);
    loopback_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::sendto(socket_, reinterpret_cast<const char*>(packet.data()),
             static_cast<int>(packet.size()), 0,
             reinterpret_cast<sockaddr*>(&loopback_addr), sizeof(loopback_addr));
}

void LanDiscovery::receive_beacons() {
    if (!is_valid(socket_) || !beacon_cb_) return;

    constexpr std::size_t BUF_SIZE = 1024;
    std::array<std::uint8_t, BUF_SIZE> buffer{};
    sockaddr_in from_addr{};
    socklen_t from_len = sizeof(from_addr);

    while (true) {
        int received = ::recvfrom(socket_, reinterpret_cast<char*>(buffer.data()),
                                  static_cast<int>(buffer.size()), 0,
                                  reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        if (received < static_cast<int>(13)) break;

        // Parse magic
        std::uint32_t magic = (static_cast<std::uint32_t>(buffer[0]) << 24)
                            | (static_cast<std::uint32_t>(buffer[1]) << 16)
                            | (static_cast<std::uint32_t>(buffer[2]) << 8)
                            | static_cast<std::uint32_t>(buffer[3]);
        if (magic != MAGIC) continue;

        // Version
        if (buffer[4] != VERSION) continue;

        // Game port
        std::uint16_t game_port = static_cast<std::uint16_t>(
            (buffer[5] << 8) | buffer[6]);

        // Player counts
        std::uint32_t max_players = (static_cast<std::uint32_t>(buffer[7]) << 24)
                                   | (static_cast<std::uint32_t>(buffer[8]) << 16)
                                   | (static_cast<std::uint32_t>(buffer[9]) << 8)
                                   | static_cast<std::uint32_t>(buffer[10]);
        std::uint32_t current_players = (static_cast<std::uint32_t>(buffer[11]) << 24)
                                       | (static_cast<std::uint32_t>(buffer[12]) << 16)
                                       | (static_cast<std::uint32_t>(buffer[13]) << 8)
                                       | static_cast<std::uint32_t>(buffer[14]);

        // Name
        std::size_t offset = 15;
        if (offset >= static_cast<std::size_t>(received)) continue;
        std::uint8_t name_len = buffer[offset++];
        if (offset + name_len > static_cast<std::size_t>(received)) continue;
        std::string name(reinterpret_cast<const char*>(buffer.data() + offset), name_len);

        // Build address from sender IP + advertised game port
        char ip_str[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from_addr.sin_addr, ip_str, sizeof(ip_str));

        // Store as IPv4-mapped IPv6 to match our Address conventions
        std::string mapped_ip = std::string("::ffff:") + ip_str;
        Beacon b;
        b.server_address = transport::Address(mapped_ip, game_port);
        b.server_name = std::move(name);
        b.max_players = max_players;
        b.current_players = current_players;
        beacon_cb_(b);
    }
}

} // namespace systems::leal::campello_net

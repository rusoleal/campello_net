#pragma once

#include "campello_net/transport/address.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Forward-declare platform socket type so the header stays light.
#ifdef _WIN32
using socket_t = std::size_t;
#else
using socket_t = int;
#endif

namespace systems::leal::campello_net {

/// LAN service discovery via UDP broadcast.
///
/// Operates on a separate IPv4 socket from the main game traffic so that
/// discovery beacons do not interfere with replication or RPCs.
///
/// Beacon wire format (UDP payload):
///   [magic:    uint32_t  'CAMP']
///   [version:  uint8_t   1]
///   [game_port:uint16_t  network byte order]
///   [max_players: uint32_t]
///   [current_players: uint32_t]
///   [name_len: uint8_t]
///   [name:     name_len bytes UTF-8]
///
/// Usage (server):
///   LanDiscovery disco;
///   disco.start_advertising(34590, 34567, "My Server");
///   while (running) { disco.poll(); sleep(1s); }
///   disco.stop_advertising();
///
/// Usage (client):
///   LanDiscovery disco;
///   disco.on_beacon_received([](const LanDiscovery::Beacon& b) {
///       connect_to(b.server_address);
///   });
///   disco.start_listening(34590);
///   while (running) { disco.poll(); }
///   disco.stop_listening();
class LanDiscovery {
public:
    static constexpr std::uint16_t DEFAULT_DISCOVERY_PORT = 34590;
    static constexpr std::uint32_t MAGIC = 0x43414D50; // 'CAMP'
    static constexpr std::uint8_t VERSION = 1;

    struct Beacon {
        transport::Address server_address; ///< Fully-formed address (includes game port)
        std::string server_name;
        std::uint32_t max_players = 0;
        std::uint32_t current_players = 0;
    };

    using BeaconCallback = std::function<void(const Beacon&)>;

    LanDiscovery();
    ~LanDiscovery();

    LanDiscovery(const LanDiscovery&) = delete;
    LanDiscovery& operator=(const LanDiscovery&) = delete;

    // ── Server-side: advertise ───────────────────────────────────────────────

    /// Start broadcasting a beacon every @p interval_seconds.
    /// @p discovery_port  Port to bind the discovery socket to.
    /// @p game_port       The port clients should actually connect to.
    /// @p server_name     Human-readable server name (max 255 bytes UTF-8).
    bool start_advertising(std::uint16_t discovery_port,
                           std::uint16_t game_port,
                           const std::string& server_name,
                           std::uint32_t max_players = 0,
                           float interval_seconds = 1.0f);

    void stop_advertising();

    /// Update current player count in the beacon.
    void set_current_players(std::uint32_t count) noexcept;

    // ── Client-side: listen ──────────────────────────────────────────────────

    /// Start listening for beacons on @p discovery_port.
    bool start_listening(std::uint16_t discovery_port);

    void stop_listening();

    void on_beacon_received(BeaconCallback cb);

    // ── Shared polling ───────────────────────────────────────────────────────

    /// Must be called regularly (e.g. every frame or every 100 ms).
    /// On the server: sends a beacon if the interval has elapsed.
    /// On the client: reads any received beacons and fires callbacks.
    void poll();

    [[nodiscard]] bool is_advertising() const noexcept;
    [[nodiscard]] bool is_listening() const noexcept;

private:
    socket_t socket_ = static_cast<socket_t>(-1);

    // Advertising state
    bool advertising_ = false;
    float advert_interval_ = 1.0f;
    float advert_accumulator_ = 0.0f;
    std::uint16_t discovery_port_ = DEFAULT_DISCOVERY_PORT;
    std::uint16_t game_port_ = 0;
    std::string server_name_;
    std::uint32_t max_players_ = 0;
    std::uint32_t current_players_ = 0;

    // Listening state
    bool listening_ = false;
    BeaconCallback beacon_cb_;

    // Timing
    std::chrono::steady_clock::time_point last_poll_ = std::chrono::steady_clock::now();

    bool create_socket(std::uint16_t port);
    void close_socket();
    void send_beacon();
    void receive_beacons();
};

} // namespace systems::leal::campello_net

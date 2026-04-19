#pragma once

#include "campello_net/network_entity.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace systems::leal::campello_net {

/// Server-side ring buffer for per-client, per-tick inputs.
///
/// The client sends serialized input commands tagged with the simulation tick
/// they were generated on. The server stores them here so that the game loop
/// can apply inputs at the exact tick they belong to, enabling:
///   - Client prediction (client runs ahead, server validates)
///   - Input replay (server rewinds and replays for lag compensation)
///   - Smooth correction (server can apply old inputs if they arrive late)
class InputBuffer {
public:
    static constexpr std::size_t MAX_TICKS = 256;

    /// Store input data for @p client at simulation tick @p tick.
    /// Older entries are overwritten once the ring buffer wraps.
    void store(ClientId client, std::uint16_t tick, std::span<const std::uint8_t> data);

    /// Retrieve input data for @p client at @p tick. Returns false if not found.
    [[nodiscard]] bool retrieve(ClientId client, std::uint16_t tick, std::vector<std::uint8_t>& out) const;

    /// Check whether input for @p client at @p tick exists.
    [[nodiscard]] bool has(ClientId client, std::uint16_t tick) const;

    /// Remove all stored inputs with tick <= @p tick for every client.
    void prune_up_to(std::uint16_t tick);

    /// Discard everything for @p client (e.g. on disconnect).
    void clear_client(ClientId client);

    /// The newest tick stored for @p client (0 if none).
    [[nodiscard]] std::uint16_t last_received_tick(ClientId client) const;

private:
    struct Entry {
        std::uint16_t tick = 0;
        std::vector<std::uint8_t> data;
    };

    // client_id -> ring buffer of entries
    std::unordered_map<ClientId, std::vector<Entry>> buffers_;
};

} // namespace systems::leal::campello_net

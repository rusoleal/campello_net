#pragma once

#include "campello_net/network_replication.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace systems::leal::campello_net {

/// Helper for server-side lag compensation.
///
/// When a client fires a weapon, the message arrives at the server after
/// roughly RTT/2.  By that time the entities have moved.  Lag compensation
/// rewinds the relevant entities to the positions they had when the client
/// pulled the trigger, performs the hit test, then restores current state.
///
/// This class is a thin wrapper around SnapshotHistory (already maintained by
/// NetworkReplicationManager).  It provides:
///   - Rewind-tick calculation from client-reported tick + RTT
///   - Retrieval of past entity states
///
/// Usage:
///   auto rewind_tick = comp.get_rewind_tick(client_reported_tick, rtt);
///   auto old_state = comp.get_entity_state(rewind_tick, target_id);
///   // Game layer: deserialize old_state, run hit-test, restore current state
class LagCompensator {
public:
    LagCompensator() = default;

    /// SnapshotHistory is owned by NetworkReplicationManager.
    void set_snapshot_history(const SnapshotHistory* history) noexcept;

    /// Server tick rate (Hz). Used for tick↔time conversions.
    void set_tick_rate(float hz) noexcept;

    /// Compute the server tick to rewind to.
    ///
    /// @param client_tick   The tick the client claims it fired on.
    /// @param client_rtt_ms Round-trip time to that client (milliseconds).
    /// @return The tick to rewind to.  If the history doesn't go back far
    ///         enough, returns the oldest available tick.
    [[nodiscard]] std::uint16_t get_rewind_tick(std::uint16_t client_tick, float client_rtt_ms) const;

    /// Retrieve the serialized state of @p entity at @p tick.
    /// Returns true and fills @p out if the snapshot exists and contains
    /// the entity.
    [[nodiscard]] bool get_entity_state(std::uint16_t tick, NetworkId entity, std::vector<std::uint8_t>& out) const;

    /// Retrieve *all* entity states stored at @p tick.
    /// Returns false if the tick is not in history.
    [[nodiscard]] bool get_all_entities(std::uint16_t tick, std::vector<EntitySnapshot>& out) const;

private:
    const SnapshotHistory* history_ = nullptr;
    float tick_rate_ = 30.0f;
};

} // namespace systems::leal::campello_net

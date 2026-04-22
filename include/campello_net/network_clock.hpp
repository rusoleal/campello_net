#pragma once

#include "campello_net/network_time.hpp"

#include <cmath>
#include <cstdint>

namespace systems::leal::campello_net {

using NetTick = std::uint32_t;

/// Simulation clock that converts wall-clock time into discrete ticks.
///
/// Features:
///   - Fixed tick rate independent of render framerate
///   - Soft alignment to remote (server) tick using NetworkTime offset
///   - Interpolation factor for render-time blending between ticks
///
/// Typical client usage:
///   clock.advance(delta_time);
///   if (clock.time_until_next_tick() <= 0.0f) { /* run simulation tick */ }
///   float t = clock.interpolation_factor();
///   // lerp visual state between tick N and N+1 using t
class NetworkClock {
public:
    explicit NetworkClock(float tick_rate = 30.0f);

    void set_tick_rate(float hz) noexcept;
    void set_network_time(const NetworkTime* net_time) noexcept;

    /// Advance local simulation time. Call once per frame.
    void advance(float delta_time) noexcept;

    /// Align local tick counter to a known server tick.
    ///
    /// @p server_tick  The tick ID from a received snapshot.
    /// @p local_time   Local monotonic time when the snapshot arrived (seconds).
    ///
    /// If the estimated drift is > 2 ticks the local counter is snapped;
    /// otherwise the existing advance() cadence is left unchanged so that
    /// correction happens gradually and imperceptibly.
    void align_to_server_tick(NetTick server_tick, double local_time) noexcept;

    /// Current local simulation tick (monotonically increasing).
    [[nodiscard]] NetTick tick() const noexcept;

    /// Fraction from 0.0 (start of current tick) to 1.0 (start of next tick).
    [[nodiscard]] float interpolation_factor() const noexcept;

    /// Time remaining until the next tick boundary (seconds).
    [[nodiscard]] float time_until_next_tick() const noexcept;

    /// Estimate the server's current tick using the smoothed clock offset.
    /// Returns the local tick if no NetworkTime has been attached.
    [[nodiscard]] NetTick estimated_server_tick(double local_time) const noexcept;

    void reset() noexcept;

private:
    float tick_rate_ = 30.0f;
    double tick_interval_ = 1.0 / 30.0;
    double accumulator_ = 0.0;
    NetTick tick_ = 0;
    const NetworkTime* net_time_ = nullptr;
};

} // namespace systems::leal::campello_net

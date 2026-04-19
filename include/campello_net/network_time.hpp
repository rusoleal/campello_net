#pragma once

#include <cstdint>

namespace systems::leal::campello_net {

/// Client-side clock synchronization helper.
///
/// Uses a simple NTP-style exchange:
///   client t0 -----> server
///   server t1 <----- server receive time
///   server t2 -----> server send time
///   client t3 <----- client receive time
///
///   offset = ((t1 - t0) + (t2 - t3)) / 2
///   rtt    = (t3 - t0) - (t2 - t1)
class NetworkTime {
public:
    NetworkTime() = default;

    /// Feed a completed sync sample. All times are in seconds (local monotonic clock).
    void record_sample(double t0, double t1, double t2, double t3) noexcept;

    /// Convert a local monotonic time to the remote (server) timeline.
    [[nodiscard]] double local_to_remote(double local_time) const noexcept;

    /// Convert a remote (server) time to the local monotonic timeline.
    [[nodiscard]] double remote_to_local(double remote_time) const noexcept;

    /// Current smoothed clock offset (remote - local). Positive means remote is ahead.
    [[nodiscard]] double offset() const noexcept { return smoothed_offset_; }

    /// Current smoothed RTT in seconds.
    [[nodiscard]] double rtt() const noexcept { return smoothed_rtt_; }

    /// Number of samples processed.
    [[nodiscard]] std::uint32_t sample_count() const noexcept { return sample_count_; }

    /// Reset all state.
    void reset() noexcept;

private:
    double smoothed_offset_ = 0.0;
    double smoothed_rtt_ = 0.0;
    std::uint32_t sample_count_ = 0;

    static constexpr double ALPHA_OFFSET = 0.1; // EMA smoothing factor for offset
    static constexpr double ALPHA_RTT = 0.2;    // EMA smoothing factor for RTT
};

} // namespace systems::leal::campello_net

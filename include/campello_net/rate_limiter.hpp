#pragma once

#include <cstddef>
#include <cstdint>

namespace systems::leal::campello_net {

/// Token-bucket rate limiter for per-client traffic shaping.
///
/// Tracks three independent limits:
///   - Messages per second
///   - Bytes per second
///   - RPCs per second
///
/// A limit of 0 (or negative) disables that check.
class RateLimiter {
public:
    RateLimiter() = default;

    /// Configure message rate limit. `rate` is sustained msgs/sec; `burst` is the
    /// maximum number of messages that can arrive in a single burst.
    void configure_messages(float rate_per_sec, float burst);

    /// Configure byte rate limit. `rate` is sustained bytes/sec; `burst` is the
    /// maximum number of bytes that can arrive in a single burst.
    void configure_bytes(float rate_per_sec, float burst);

    /// Configure RPC rate limit. `rate` is sustained RPCs/sec; `burst` is the
    /// maximum number of RPCs that can arrive in a single burst.
    void configure_rpcs(float rate_per_sec, float burst);

    /// Check whether a message of `byte_size` is within limits.
    /// Returns true if allowed, false if any limit is exceeded.
    bool allow_message(std::size_t byte_size);

    /// Check whether an RPC is within limits.
    /// Returns true if allowed, false if the RPC limit is exceeded.
    bool allow_rpc();

    struct Stats {
        std::uint64_t messages_allowed = 0;
        std::uint64_t messages_dropped = 0;
        std::uint64_t bytes_allowed = 0;
        std::uint64_t bytes_dropped = 0;
        std::uint64_t rpcs_allowed = 0;
        std::uint64_t rpcs_dropped = 0;
    };

    [[nodiscard]] Stats stats() const noexcept;
    void reset_stats() noexcept;

private:
    struct Bucket {
        float rate_per_sec = 0.0f;
        float burst = 0.0f;
        float tokens = 0.0f;
        double last_update = 0.0;

        void configure(float rate, float burst_size);
        bool consume(float amount);
    };

    Bucket msg_bucket_;
    Bucket byte_bucket_;
    Bucket rpc_bucket_;
    Stats stats_;
};

} // namespace systems::leal::campello_net

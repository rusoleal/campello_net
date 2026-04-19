#include "campello_net/rate_limiter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace systems::leal::campello_net {

// ── Token Bucket helpers ────────────────────────────────────────────────────

namespace {

[[nodiscard]] double now_seconds() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

} // anonymous namespace

void RateLimiter::Bucket::configure(float rate, float burst_size) {
    rate_per_sec = std::max(0.0f, rate);
    burst = std::max(0.0f, burst_size);
    tokens = burst; // start with a full bucket
    last_update = now_seconds();
}

bool RateLimiter::Bucket::consume(float amount) {
    if (rate_per_sec <= 0.0f && burst <= 0.0f) {
        return true; // disabled
    }

    double t = now_seconds();
    double elapsed = t - last_update;
    last_update = t;

    tokens += static_cast<float>(rate_per_sec * elapsed);
    if (tokens > burst) {
        tokens = burst;
    }

    if (tokens >= amount) {
        tokens -= amount;
        return true;
    }
    return false;
}

// ── RateLimiter public methods ──────────────────────────────────────────────

void RateLimiter::configure_messages(float rate_per_sec, float burst) {
    msg_bucket_.configure(rate_per_sec, burst);
}

void RateLimiter::configure_bytes(float rate_per_sec, float burst) {
    byte_bucket_.configure(rate_per_sec, burst);
}

void RateLimiter::configure_rpcs(float rate_per_sec, float burst) {
    rpc_bucket_.configure(rate_per_sec, burst);
}

bool RateLimiter::allow_message(std::size_t byte_size) {
    bool msg_ok = msg_bucket_.consume(1.0f);
    bool byte_ok = byte_bucket_.consume(static_cast<float>(byte_size));

    if (msg_ok && byte_ok) {
        ++stats_.messages_allowed;
        stats_.bytes_allowed += byte_size;
        return true;
    }

    ++stats_.messages_dropped;
    stats_.bytes_dropped += byte_size;
    return false;
}

bool RateLimiter::allow_rpc() {
    bool ok = rpc_bucket_.consume(1.0f);
    if (ok) {
        ++stats_.rpcs_allowed;
    } else {
        ++stats_.rpcs_dropped;
    }
    return ok;
}

RateLimiter::Stats RateLimiter::stats() const noexcept {
    return stats_;
}

void RateLimiter::reset_stats() noexcept {
    stats_ = Stats{};
}

} // namespace systems::leal::campello_net

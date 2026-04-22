#include "campello_net/network_clock.hpp"

namespace systems::leal::campello_net {

NetworkClock::NetworkClock(float tick_rate) {
    set_tick_rate(tick_rate);
}

void NetworkClock::set_tick_rate(float hz) noexcept {
    tick_rate_ = hz;
    tick_interval_ = (hz > 0.0f) ? (1.0 / static_cast<double>(hz)) : 0.0;
}

void NetworkClock::set_network_time(const NetworkTime* net_time) noexcept {
    net_time_ = net_time;
}

void NetworkClock::advance(float delta_time) noexcept {
    accumulator_ += static_cast<double>(delta_time);
    while (accumulator_ + 1e-9 >= tick_interval_) {
        accumulator_ -= tick_interval_;
        ++tick_;
    }
}

void NetworkClock::align_to_server_tick(NetTick server_tick, double local_time) noexcept {
    if (!net_time_) {
        tick_ = server_tick;
        accumulator_ = 0.0;
        return;
    }

    const double server_time_now = net_time_->local_to_remote(local_time);
    const double one_way = net_time_->rtt() * 0.5;

    // server_tick was sent one_way seconds ago.
    // Current server tick ≈ server_tick + one_way / tick_interval.
    const NetTick expected = server_tick + static_cast<NetTick>(one_way / tick_interval_ + 0.5);

    const std::int64_t drift = static_cast<std::int64_t>(expected) - static_cast<std::int64_t>(tick_);

    if (std::abs(drift) > 2) {
        // Large drift — snap to avoid desync.
        tick_ = expected;
        accumulator_ = 0.0;
    } else if (drift > 0) {
        // Slightly behind — accelerate by advancing extra sub-tick time.
        accumulator_ += tick_interval_ * 0.5f;
        if (accumulator_ >= tick_interval_) {
            accumulator_ -= tick_interval_;
            ++tick_;
        }
    } else if (drift < 0) {
        // Slightly ahead — decelerate.
        accumulator_ -= tick_interval_ * 0.5f;
    }
}

NetTick NetworkClock::tick() const noexcept {
    return tick_;
}

float NetworkClock::interpolation_factor() const noexcept {
    if (tick_interval_ <= 0.0)
        return 0.0f;
    return static_cast<float>(accumulator_ / tick_interval_);
}

float NetworkClock::time_until_next_tick() const noexcept {
    return static_cast<float>(tick_interval_ - accumulator_);
}

NetTick NetworkClock::estimated_server_tick(double local_time) const noexcept {
    if (!net_time_ || tick_interval_ <= 0.0)
        return tick_;

    const double server_time = net_time_->local_to_remote(local_time);
    return static_cast<NetTick>(server_time / tick_interval_);
}

void NetworkClock::reset() noexcept {
    accumulator_ = 0.0;
    tick_ = 0;
}

} // namespace systems::leal::campello_net

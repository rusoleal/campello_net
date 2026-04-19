#include "campello_net/prediction/lag_compensator.hpp"

#include <cmath>

namespace systems::leal::campello_net {

void LagCompensator::set_snapshot_history(const SnapshotHistory* history) noexcept {
    history_ = history;
}

void LagCompensator::set_tick_rate(float hz) noexcept {
    tick_rate_ = hz;
}

std::uint16_t LagCompensator::get_rewind_tick(std::uint16_t client_tick,
                                               float client_rtt_ms) const {
    if (tick_rate_ <= 0.0f) return client_tick;

    float tick_interval_ms = 1000.0f / tick_rate_;
    // One-way latency ≈ RTT/2.  Round to nearest tick.
    int tick_offset = static_cast<int>(std::round(client_rtt_ms * 0.5f / tick_interval_ms));

    std::int32_t result = static_cast<std::int32_t>(client_tick) - tick_offset;
    if (result < 0) result = 0;
    return static_cast<std::uint16_t>(result);
}

bool LagCompensator::get_entity_state(std::uint16_t tick, NetworkId entity,
                                      std::vector<std::uint8_t>& out) const {
    if (!history_) return false;

    const auto* snap = history_->retrieve(tick);
    if (!snap) return false;

    for (const auto& ent : *snap) {
        if (ent.id == entity) {
            out = ent.data;
            return true;
        }
    }
    return false;
}

bool LagCompensator::get_all_entities(std::uint16_t tick,
                                      std::vector<EntitySnapshot>& out) const {
    if (!history_) return false;

    const auto* snap = history_->retrieve(tick);
    if (!snap) return false;

    out = *snap;
    return true;
}

} // namespace systems::leal::campello_net

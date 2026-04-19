#include "campello_net/prediction/input_buffer.hpp"

#include <cstring>

namespace systems::leal::campello_net {

void InputBuffer::store(ClientId client, std::uint16_t tick, std::span<const std::uint8_t> data) {
    auto& buf = buffers_[client];
    if (buf.empty()) {
        buf.resize(MAX_TICKS);
    }
    std::size_t idx = tick % MAX_TICKS;
    buf[idx].tick = tick;
    buf[idx].data.assign(data.begin(), data.end());
}

bool InputBuffer::retrieve(ClientId client, std::uint16_t tick, std::vector<std::uint8_t>& out) const {
    auto it = buffers_.find(client);
    if (it == buffers_.end() || it->second.empty())
        return false;

    const auto& buf = it->second;
    std::size_t idx = tick % MAX_TICKS;
    if (buf[idx].tick != tick)
        return false;

    out = buf[idx].data;
    return true;
}

bool InputBuffer::has(ClientId client, std::uint16_t tick) const {
    auto it = buffers_.find(client);
    if (it == buffers_.end() || it->second.empty())
        return false;

    const auto& buf = it->second;
    std::size_t idx = tick % MAX_TICKS;
    return buf[idx].tick == tick;
}

void InputBuffer::prune_up_to(std::uint16_t tick) {
    for (auto& [client, buf] : buffers_) {
        if (buf.empty())
            continue;
        for (auto& entry : buf) {
            if (entry.tick != 0 && static_cast<std::int16_t>(tick - entry.tick) >= 0) {
                entry.tick = 0;
                entry.data.clear();
            }
        }
    }
}

void InputBuffer::clear_client(ClientId client) {
    buffers_.erase(client);
}

std::uint16_t InputBuffer::last_received_tick(ClientId client) const {
    auto it = buffers_.find(client);
    if (it == buffers_.end() || it->second.empty())
        return 0;

    std::uint16_t max_tick = 0;
    for (const auto& entry : it->second) {
        if (entry.tick != 0 && static_cast<std::int16_t>(entry.tick - max_tick) > 0) {
            max_tick = entry.tick;
        }
    }
    return max_tick;
}

} // namespace systems::leal::campello_net

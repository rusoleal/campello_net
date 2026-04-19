#include "campello_net/transport/network_simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

namespace systems::leal::campello_net::transport {

struct NetworkSimulator::Impl {
    std::unique_ptr<ITransport> inner;
    float packet_loss = 0.0f;
    float latency_min_ms = 0.0f;
    float latency_max_ms = 0.0f;
    float jitter_ms = 0.0f;
    float duplication = 0.0f;

    struct QueuedPacket {
        double deliver_time;
        std::vector<uint8_t> data;
        Address sender;
        Address target;
        PacketReliability reliability;
        bool outbound;
        bool targeted = false;
    };

    std::vector<QueuedPacket> delay_queue;
    std::vector<QueuedPacket> ready_queue;
    std::mt19937 rng{42};

    [[nodiscard]] double now() const {
        auto t = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(t.time_since_epoch()).count();
    }

    [[nodiscard]] bool should_drop() {
        if (packet_loss <= 0.0f) return false;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(rng) < packet_loss;
    }

    [[nodiscard]] bool should_duplicate() {
        if (duplication <= 0.0f) return false;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(rng) < duplication;
    }

    [[nodiscard]] double calculate_delay_s() {
        double delay_ms = latency_min_ms;
        if (latency_max_ms > latency_min_ms) {
            std::uniform_real_distribution<float> dist(latency_min_ms, latency_max_ms);
            delay_ms = dist(rng);
        }
        if (jitter_ms > 0.0f) {
            std::uniform_real_distribution<float> dist(-jitter_ms, jitter_ms);
            delay_ms += dist(rng);
        }
        return std::max(0.0, delay_ms) / 1000.0;
    }
};

NetworkSimulator::NetworkSimulator(std::unique_ptr<ITransport> inner)
    : impl_(std::make_unique<Impl>()) {
    impl_->inner = std::move(inner);
}

NetworkSimulator::~NetworkSimulator() = default;
NetworkSimulator::NetworkSimulator(NetworkSimulator&&) noexcept = default;
NetworkSimulator& NetworkSimulator::operator=(NetworkSimulator&&) noexcept = default;

bool NetworkSimulator::bind(const Address& address) {
    return impl_->inner->bind(address);
}

bool NetworkSimulator::connect(const Address& address) {
    return impl_->inner->connect(address);
}

void NetworkSimulator::disconnect() {
    impl_->inner->disconnect();
    impl_->delay_queue.clear();
    impl_->ready_queue.clear();
}

bool NetworkSimulator::is_connected() const noexcept {
    return impl_->inner->is_connected();
}

bool NetworkSimulator::send(const uint8_t* data, std::size_t length, PacketReliability reliability) {
    if (impl_->should_drop()) return true; // drop silently

    double deliver = impl_->now() + impl_->calculate_delay_s();

    Impl::QueuedPacket pkt;
    pkt.deliver_time = deliver;
    pkt.data.assign(data, data + length);
    pkt.reliability = reliability;
    pkt.outbound = true;
    impl_->delay_queue.push_back(std::move(pkt));

    if (impl_->should_duplicate()) {
        Impl::QueuedPacket dup = impl_->delay_queue.back();
        dup.deliver_time = deliver + (impl_->jitter_ms / 1000.0);
        impl_->delay_queue.push_back(std::move(dup));
    }
    return true;
}

bool NetworkSimulator::send_to(const Address& address, const uint8_t* data, std::size_t length,
                               PacketReliability reliability) {
    if (impl_->should_drop()) return true; // drop silently

    double deliver = impl_->now() + impl_->calculate_delay_s();

    Impl::QueuedPacket pkt;
    pkt.deliver_time = deliver;
    pkt.data.assign(data, data + length);
    pkt.target = address;
    pkt.targeted = true;
    pkt.reliability = reliability;
    pkt.outbound = true;
    impl_->delay_queue.push_back(std::move(pkt));

    if (impl_->should_duplicate()) {
        Impl::QueuedPacket dup = impl_->delay_queue.back();
        dup.deliver_time = deliver + (impl_->jitter_ms / 1000.0);
        impl_->delay_queue.push_back(std::move(dup));
    }
    return true;
}

void NetworkSimulator::poll() {
    impl_->inner->poll();

    // Inbound: intercept packets from inner transport and delay them.
    while (true) {
        uint8_t buffer[2048];
        std::size_t len = 0;
        Address sender;
        if (!impl_->inner->pop_receive(buffer, sizeof(buffer), len, sender)) break;

        if (impl_->should_drop()) continue;

        double deliver = impl_->now() + impl_->calculate_delay_s();
        Impl::QueuedPacket pkt;
        pkt.deliver_time = deliver;
        pkt.data.assign(buffer, buffer + len);
        pkt.sender = sender;
        pkt.outbound = false;
        impl_->delay_queue.push_back(std::move(pkt));

        if (impl_->should_duplicate()) {
            Impl::QueuedPacket dup = impl_->delay_queue.back();
            dup.deliver_time += (impl_->jitter_ms / 1000.0);
            impl_->delay_queue.push_back(std::move(dup));
        }
    }

    // Move packets whose delay has expired.
    double now = impl_->now();
    for (auto it = impl_->delay_queue.begin(); it != impl_->delay_queue.end();) {
        if (it->deliver_time <= now) {
            if (it->outbound) {
                if (it->targeted) {
                    impl_->inner->send_to(it->target, it->data.data(), it->data.size(), it->reliability);
                } else {
                    impl_->inner->send(it->data.data(), it->data.size(), it->reliability);
                }
            } else {
                impl_->ready_queue.push_back(std::move(*it));
            }
            it = impl_->delay_queue.erase(it);
        } else {
            ++it;
        }
    }
}

bool NetworkSimulator::pop_receive(uint8_t* buffer, std::size_t max_length, std::size_t& out_length,
                                    Address& out_sender) {
    if (impl_->ready_queue.empty()) return false;

    auto& pkt = impl_->ready_queue.front();
    out_length = std::min(max_length, pkt.data.size());
    std::memcpy(buffer, pkt.data.data(), out_length);
    out_sender = pkt.sender;
    impl_->ready_queue.erase(impl_->ready_queue.begin());
    return true;
}

float NetworkSimulator::rtt() const noexcept {
    return impl_->inner->rtt();
}

float NetworkSimulator::packet_loss() const noexcept {
    return impl_->packet_loss;
}

float NetworkSimulator::get_connection_rtt(const Address& address) const noexcept {
    return impl_->inner->get_connection_rtt(address);
}

float NetworkSimulator::get_connection_packet_loss(const Address& address) const noexcept {
    return impl_->inner->get_connection_packet_loss(address);
}

void NetworkSimulator::set_packet_loss(float ratio) {
    impl_->packet_loss = std::clamp(ratio, 0.0f, 1.0f);
}

void NetworkSimulator::set_latency(float min_ms, float max_ms) {
    impl_->latency_min_ms = min_ms;
    impl_->latency_max_ms = max_ms;
}

void NetworkSimulator::set_jitter(float ms) {
    impl_->jitter_ms = ms;
}

void NetworkSimulator::set_duplication(float ratio) {
    impl_->duplication = std::clamp(ratio, 0.0f, 1.0f);
}

} // namespace systems::leal::campello_net::transport

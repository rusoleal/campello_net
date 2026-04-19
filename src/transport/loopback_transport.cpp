#include "campello_net/transport/loopback_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <vector>

namespace systems::leal::campello_net::transport {

// ── LoopbackHub::Impl ───────────────────────────────────────────────────────

struct LoopbackHub::Impl {
    std::mutex mutex_;
    std::map<Address, LoopbackTransport*> servers_;
    std::map<Address, LoopbackTransport*> clients_;
    std::map<Address, Address> client_to_server_;
    std::uint16_t next_port_ = 50000;

    LoopbackTransport* find_transport(const Address& addr) {
        auto it = clients_.find(addr);
        if (it != clients_.end()) return it->second;
        auto sit = servers_.find(addr);
        if (sit != servers_.end()) return sit->second;
        return nullptr;
    }

    static void deliver_to(LoopbackTransport* target, const Address& from,
                           const uint8_t* data, std::size_t len, PacketReliability reliability) {
        if (!target) return;
        target->enqueue_pending(from, data, len, reliability);
    }
};

LoopbackHub::LoopbackHub() : impl_(std::make_unique<Impl>()) {}
LoopbackHub::~LoopbackHub() = default;

// ── LoopbackHub methods ─────────────────────────────────────────────────────

void LoopbackHub::bind_server(const Address& addr, LoopbackTransport* t) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->servers_[addr] = t;
}

void LoopbackHub::unbind_server(const Address& addr) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->servers_.erase(addr);
    auto it = impl_->client_to_server_.begin();
    while (it != impl_->client_to_server_.end()) {
        if (it->second == addr) {
            impl_->clients_.erase(it->first);
            it = impl_->client_to_server_.erase(it);
        } else {
            ++it;
        }
    }
}

bool LoopbackHub::connect_client(const Address& server_addr, LoopbackTransport* client,
                                  Address& out_client_addr) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    auto it = impl_->servers_.find(server_addr);
    if (it == impl_->servers_.end()) {
        return false;
    }

    Address client_addr;
    do {
        client_addr = Address("127.0.0.1", impl_->next_port_++);
    } while (impl_->clients_.find(client_addr) != impl_->clients_.end());

    impl_->clients_[client_addr] = client;
    impl_->client_to_server_[client_addr] = server_addr;
    out_client_addr = client_addr;
    return true;
}

void LoopbackHub::disconnect_client(const Address& client_addr) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->clients_.erase(client_addr);
    impl_->client_to_server_.erase(client_addr);
}

void LoopbackHub::deliver(const Address& to, const Address& from,
                          const uint8_t* data, std::size_t len, PacketReliability reliability) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    LoopbackTransport* target = impl_->find_transport(to);
    if (!target) return;
    Impl::deliver_to(target, from, data, len, reliability);
}

void LoopbackHub::broadcast(const Address& server_addr, const Address& from,
                            const uint8_t* data, std::size_t len, PacketReliability reliability) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    for (const auto& [client_addr, srv_addr] : impl_->client_to_server_) {
        if (srv_addr != server_addr) continue;
        auto it = impl_->clients_.find(client_addr);
        if (it == impl_->clients_.end()) continue;
        if (!it->second) continue;
        Impl::deliver_to(it->second, from, data, len, reliability);
    }
}

// ── LoopbackTransport::Impl ─────────────────────────────────────────────────

struct LoopbackTransport::Impl {
    std::shared_ptr<LoopbackHub> hub;

    bool bound = false;
    bool connected = false;
    Address local_addr;
    Address remote_addr;

    struct Packet {
        Address sender;
        std::vector<std::uint8_t> data;
        PacketReliability reliability;
    };
    std::vector<Packet> pending_recv;
    std::vector<Packet> ready_recv;

    float latency_sec = 0.0f;
    float packet_loss_ratio = 0.0f;

    std::mt19937 rng{42};

    [[nodiscard]] bool should_drop() {
        if (packet_loss_ratio <= 0.0f) return false;
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        return dist(rng) < packet_loss_ratio;
    }

    [[nodiscard]] double now() const {
        auto t = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(t.time_since_epoch()).count();
    }

    struct DelayedPacket {
        double delivery_time;
        Address recipient;
        Address sender;
        std::vector<std::uint8_t> data;
        PacketReliability reliability;
        bool broadcast = false;
    };
    std::vector<DelayedPacket> delayed_outbound;
};

// ── LoopbackTransport public methods ────────────────────────────────────────

LoopbackTransport::LoopbackTransport(std::shared_ptr<LoopbackHub> hub)
    : impl_(std::make_unique<Impl>()) {
    if (hub) {
        impl_->hub = std::move(hub);
    } else {
        impl_->hub = std::make_shared<LoopbackHub>();
    }
}

LoopbackTransport::~LoopbackTransport() {
    if (impl_) {
        disconnect();
    }
}

LoopbackTransport::LoopbackTransport(LoopbackTransport&&) noexcept = default;
LoopbackTransport& LoopbackTransport::operator=(LoopbackTransport&&) noexcept = default;

bool LoopbackTransport::bind(const Address& address) {
    if (impl_->bound) return false;
    impl_->local_addr = address;
    impl_->hub->bind_server(address, this);
    impl_->bound = true;
    return true;
}

bool LoopbackTransport::connect(const Address& address) {
    if (impl_->connected || impl_->bound) return false;
    Address client_addr;
    if (!impl_->hub->connect_client(address, this, client_addr)) {
        return false;
    }
    impl_->local_addr = client_addr;
    impl_->remote_addr = address;
    impl_->connected = true;
    return true;
}

void LoopbackTransport::disconnect() {
    if (impl_->bound) {
        impl_->hub->unbind_server(impl_->local_addr);
        impl_->bound = false;
    }
    if (impl_->connected) {
        impl_->hub->disconnect_client(impl_->local_addr);
        impl_->connected = false;
    }
    impl_->local_addr = Address{};
    impl_->remote_addr = Address{};
    impl_->pending_recv.clear();
    impl_->ready_recv.clear();
}

bool LoopbackTransport::is_connected() const noexcept {
    return impl_->bound || impl_->connected;
}

bool LoopbackTransport::send(const uint8_t* data, std::size_t length, PacketReliability reliability) {
    if (impl_->should_drop()) return true; // silent drop

    if (impl_->latency_sec > 0.0f) {
        Impl::DelayedPacket dp;
        dp.delivery_time = impl_->now() + impl_->latency_sec;
        dp.sender = impl_->local_addr;
        dp.data.assign(data, data + length);
        dp.reliability = reliability;
        if (impl_->bound) {
            dp.broadcast = true;
            dp.recipient = impl_->local_addr;
        } else {
            dp.recipient = impl_->remote_addr;
        }
        impl_->delayed_outbound.push_back(std::move(dp));
        return true;
    }

    if (impl_->bound) {
        impl_->hub->broadcast(impl_->local_addr, impl_->local_addr,
                              data, length, reliability);
        return true;
    }
    if (impl_->connected) {
        impl_->hub->deliver(impl_->remote_addr, impl_->local_addr,
                            data, length, reliability);
        return true;
    }
    return false;
}

bool LoopbackTransport::send_to(const Address& address, const uint8_t* data,
                                std::size_t length, PacketReliability reliability) {
    if (!impl_->bound) return false;
    if (impl_->should_drop()) return true; // silent drop

    if (impl_->latency_sec > 0.0f) {
        Impl::DelayedPacket dp;
        dp.delivery_time = impl_->now() + impl_->latency_sec;
        dp.recipient = address;
        dp.sender = impl_->local_addr;
        dp.data.assign(data, data + length);
        dp.reliability = reliability;
        impl_->delayed_outbound.push_back(std::move(dp));
        return true;
    }

    impl_->hub->deliver(address, impl_->local_addr, data, length, reliability);
    return true;
}

void LoopbackTransport::poll() {
    // Flush delayed outbound packets whose time has come
    double now = impl_->now();
    for (auto it = impl_->delayed_outbound.begin(); it != impl_->delayed_outbound.end();) {
        if (it->delivery_time <= now) {
            if (it->broadcast) {
                impl_->hub->broadcast(it->sender, it->sender,
                                      it->data.data(), it->data.size(), it->reliability);
            } else {
                impl_->hub->deliver(it->recipient, it->sender,
                                    it->data.data(), it->data.size(), it->reliability);
            }
            it = impl_->delayed_outbound.erase(it);
        } else {
            ++it;
        }
    }

    if (!impl_->pending_recv.empty()) {
        impl_->ready_recv.insert(impl_->ready_recv.end(),
                                 std::make_move_iterator(impl_->pending_recv.begin()),
                                 std::make_move_iterator(impl_->pending_recv.end()));
        impl_->pending_recv.clear();
    }
}

bool LoopbackTransport::pop_receive(uint8_t* buffer, std::size_t max_length,
                                    std::size_t& out_length, Address& out_sender) {
    if (impl_->ready_recv.empty()) return false;

    auto& pkt = impl_->ready_recv.front();
    out_length = std::min(max_length, pkt.data.size());
    std::memcpy(buffer, pkt.data.data(), out_length);
    out_sender = pkt.sender;
    impl_->ready_recv.erase(impl_->ready_recv.begin());
    return true;
}

float LoopbackTransport::rtt() const noexcept {
    return impl_->latency_sec * 2.0f;
}

float LoopbackTransport::packet_loss() const noexcept {
    return impl_->packet_loss_ratio;
}

float LoopbackTransport::get_connection_rtt(const Address& address) const noexcept {
    (void)address;
    return impl_->latency_sec * 2.0f;
}

float LoopbackTransport::get_connection_packet_loss(const Address& address) const noexcept {
    (void)address;
    return impl_->packet_loss_ratio;
}

void LoopbackTransport::set_latency(float seconds) noexcept {
    impl_->latency_sec = std::max(0.0f, seconds);
}

void LoopbackTransport::set_packet_loss(float ratio) noexcept {
    impl_->packet_loss_ratio = std::clamp(ratio, 0.0f, 1.0f);
}

void LoopbackTransport::enqueue_pending(const Address& sender, const uint8_t* data,
                                         std::size_t len, PacketReliability reliability) {
    Impl::Packet pkt;
    pkt.sender = sender;
    pkt.data.assign(data, data + len);
    pkt.reliability = reliability;
    impl_->pending_recv.push_back(std::move(pkt));
}

} // namespace systems::leal::campello_net::transport

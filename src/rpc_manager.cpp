#include "campello_net/rpc_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace systems::leal::campello_net {

namespace {

[[nodiscard]] double now_seconds() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

} // anonymous namespace

// ── Configuration ───────────────────────────────────────────────────────────

void RpcManager::set_network_manager(NetworkManager* net) noexcept {
    net_ = net;
}

void RpcManager::set_entity_manager(NetworkEntityManager* mgr) noexcept {
    entity_mgr_ = mgr;
}

void RpcManager::set_max_payload_size(std::size_t max) noexcept {
    max_payload_size_ = max;
}

void RpcManager::register_handler(std::uint16_t rpc_id, Handler handler, RpcAuthority authority) {
    handlers_[rpc_id] = HandlerEntry{std::move(handler), authority};
}

void RpcManager::unregister_handler(std::uint16_t rpc_id) {
    handlers_.erase(rpc_id);
}

void RpcManager::set_rpc_rate_limit(std::uint16_t rpc_id, float max_per_sec, float burst) {
    auto& bucket = rpc_rate_buckets_[rpc_id];
    bucket.rate_per_sec = std::max(0.0f, max_per_sec);
    bucket.burst = std::max(0.0f, burst);
    bucket.tokens = bucket.burst;
    bucket.last_update = now_seconds();
}

// ── Low-level invocation ────────────────────────────────────────────────────

void RpcManager::invoke_client(ClientId client, std::uint16_t rpc_id, const serialization::BitStream& args) {
    send_rpc(client, rpc_id, args);
}

void RpcManager::invoke_server(std::uint16_t rpc_id, const serialization::BitStream& args) {
    send_rpc(0, rpc_id, args);
}

void RpcManager::invoke_broadcast(std::uint16_t rpc_id, const serialization::BitStream& args) {
    if (!net_)
        return;

    auto packet = build_rpc_packet(rpc_id, args);
    net_->broadcast(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
}

void RpcManager::invoke_owner(std::uint16_t rpc_id, NetworkId entity_id, const serialization::BitStream& args) {
    if (!entity_mgr_)
        return;

    ClientId owner = entity_mgr_->owner(entity_id);
    if (owner == 0)
        return; // Server-owned; no client owner to notify.

    send_rpc(owner, rpc_id, args);
}

void RpcManager::invoke_not_owner(std::uint16_t rpc_id, NetworkId entity_id, const serialization::BitStream& args) {
    if (!net_ || !entity_mgr_)
        return;

    ClientId owner = entity_mgr_->owner(entity_id);
    auto packet = build_rpc_packet(rpc_id, args);
    if (packet.empty())
        return; // oversized
    net_->broadcast(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered, owner);
}

// ── Internal helpers ────────────────────────────────────────────────────────

std::vector<std::uint8_t> RpcManager::build_rpc_packet(std::uint16_t rpc_id,
                                                       const serialization::BitStream& args) const {
    auto span = args.span();
    if (max_payload_size_ > 0 && span.size() > max_payload_size_) {
        return {}; // oversized — silently dropped
    }

    std::vector<std::uint8_t> packet(2 + span.size());
    packet[0] = static_cast<std::uint8_t>(rpc_id >> 8);
    packet[1] = static_cast<std::uint8_t>(rpc_id & 0xFF);
    if (!span.empty()) {
        std::memcpy(packet.data() + 2, span.data(), span.size());
    }

    // System message envelope
    std::vector<std::uint8_t> sys_msg(3 + packet.size());
    sys_msg[0] = 0xCA;
    sys_msg[1] = 0xFE;
    sys_msg[2] = 0x22; // Rpc
    std::memcpy(sys_msg.data() + 3, packet.data(), packet.size());

    return sys_msg;
}

void RpcManager::send_rpc(ClientId target_client, std::uint16_t rpc_id, const serialization::BitStream& args) {
    if (!net_)
        return;

    auto packet = build_rpc_packet(rpc_id, args);
    if (packet.empty())
        return; // oversized

    if (net_->mode() == NetworkManager::Mode::Server || net_->mode() == NetworkManager::Mode::Host) {
        net_->send(target_client, packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
    } else if (net_->mode() == NetworkManager::Mode::Client) {
        net_->send(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
    }
}

// ── Receiving ───────────────────────────────────────────────────────────────

void RpcManager::on_receive(ClientId sender, const std::uint8_t* data, std::size_t len) {
    if (len < 2)
        return;

    if (max_payload_size_ > 0 && (len - 2) > max_payload_size_)
        return; // oversized payload

    std::uint16_t rpc_id = static_cast<std::uint16_t>((data[0] << 8) | data[1]);

    auto it = handlers_.find(rpc_id);
    if (it == handlers_.end())
        return;

    // Authority check
    if (it->second.authority == RpcAuthority::ServerOnly && sender != 0) {
        return; // Reject — only server can invoke this RPC
    }

    // Per-RPC type rate limit
    auto rate_it = rpc_rate_buckets_.find(rpc_id);
    if (rate_it != rpc_rate_buckets_.end()) {
        auto& bucket = rate_it->second;
        if (bucket.rate_per_sec > 0.0f) {
            double t = now_seconds();
            double elapsed = t - bucket.last_update;
            bucket.last_update = t;
            bucket.tokens = std::min(bucket.burst, bucket.tokens + bucket.rate_per_sec * static_cast<float>(elapsed));
            if (bucket.tokens < 1.0f) {
                return; // rate limited
            }
            bucket.tokens -= 1.0f;
        }
    }

    // Build RpcParams
    RpcParams params;
    params.sender = sender;
    if (net_) {
        params.server_timestamp = net_->network_time();
        params.sender_rtt = net_->client_rtt(sender);
    }

    serialization::BitStream stream(std::span<const std::uint8_t>(data + 2, len - 2));
    it->second.handler(params, stream);
}

} // namespace systems::leal::campello_net

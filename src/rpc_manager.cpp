#include "campello_net/rpc_manager.hpp"

#include <cstring>

namespace systems::leal::campello_net {

// ── Configuration ───────────────────────────────────────────────────────────

void RpcManager::set_network_manager(NetworkManager* net) noexcept {
    net_ = net;
}

void RpcManager::register_handler(std::uint16_t rpc_id, Handler handler) {
    handlers_[rpc_id] = std::move(handler);
}

void RpcManager::unregister_handler(std::uint16_t rpc_id) {
    handlers_.erase(rpc_id);
}

// ── Low-level invocation ────────────────────────────────────────────────────

void RpcManager::invoke_client(ClientId client, std::uint16_t rpc_id,
                                 const serialization::BitStream& args) {
    send_rpc(client, rpc_id, args);
}

void RpcManager::invoke_server(std::uint16_t rpc_id, const serialization::BitStream& args) {
    send_rpc(0, rpc_id, args);
}

// ── Internal send ───────────────────────────────────────────────────────────

void RpcManager::send_rpc(ClientId target_client, std::uint16_t rpc_id,
                          const serialization::BitStream& args) {
    if (!net_) return;

    auto span = args.span();
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

    if (net_->mode() == NetworkManager::Mode::Server ||
        net_->mode() == NetworkManager::Mode::Host) {
        net_->send(target_client, sys_msg.data(), sys_msg.size(),
                   transport::PacketReliability::ReliableOrdered);
    } else if (net_->mode() == NetworkManager::Mode::Client) {
        net_->send(sys_msg.data(), sys_msg.size(),
                   transport::PacketReliability::ReliableOrdered);
    }
}

// ── Receiving ───────────────────────────────────────────────────────────────

void RpcManager::on_receive(ClientId sender, const std::uint8_t* data, std::size_t len) {
    if (len < 2) return;

    std::uint16_t rpc_id = static_cast<std::uint16_t>((data[0] << 8) | data[1]);

    auto it = handlers_.find(rpc_id);
    if (it == handlers_.end()) return;

    serialization::BitStream stream(std::span<const std::uint8_t>(data + 2, len - 2));
    it->second(sender, stream);
}

} // namespace systems::leal::campello_net

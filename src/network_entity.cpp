#include "campello_net/network_entity.hpp"

#include <algorithm>
#include <cstring>

namespace systems::leal::campello_net {

// ── Big-endian helpers ──────────────────────────────────────────────────────

void NetworkEntityManager::write_u64_be(std::uint8_t* dst, std::uint64_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value >> 56);
    dst[1] = static_cast<std::uint8_t>(value >> 48);
    dst[2] = static_cast<std::uint8_t>(value >> 40);
    dst[3] = static_cast<std::uint8_t>(value >> 32);
    dst[4] = static_cast<std::uint8_t>(value >> 24);
    dst[5] = static_cast<std::uint8_t>(value >> 16);
    dst[6] = static_cast<std::uint8_t>(value >> 8);
    dst[7] = static_cast<std::uint8_t>(value);
}

void NetworkEntityManager::write_u32_be(std::uint8_t* dst, std::uint32_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value >> 24);
    dst[1] = static_cast<std::uint8_t>(value >> 16);
    dst[2] = static_cast<std::uint8_t>(value >> 8);
    dst[3] = static_cast<std::uint8_t>(value);
}

void NetworkEntityManager::write_u16_be(std::uint8_t* dst, std::uint16_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value >> 8);
    dst[1] = static_cast<std::uint8_t>(value);
}

std::uint64_t NetworkEntityManager::read_u64_be(const std::uint8_t* src) noexcept {
    return (static_cast<std::uint64_t>(src[0]) << 56) | (static_cast<std::uint64_t>(src[1]) << 48) |
           (static_cast<std::uint64_t>(src[2]) << 40) | (static_cast<std::uint64_t>(src[3]) << 32) |
           (static_cast<std::uint64_t>(src[4]) << 24) | (static_cast<std::uint64_t>(src[5]) << 16) |
           (static_cast<std::uint64_t>(src[6]) << 8) | static_cast<std::uint64_t>(src[7]);
}

std::uint32_t NetworkEntityManager::read_u32_be(const std::uint8_t* src) noexcept {
    return (static_cast<std::uint32_t>(src[0]) << 24) | (static_cast<std::uint32_t>(src[1]) << 16) |
           (static_cast<std::uint32_t>(src[2]) << 8) | static_cast<std::uint32_t>(src[3]);
}

std::uint16_t NetworkEntityManager::read_u16_be(const std::uint8_t* src) noexcept {
    return static_cast<std::uint16_t>((src[0] << 8) | src[1]);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

NetworkEntityManager::~NetworkEntityManager() {
    if (bridge_) {
        for (auto& [id, ent] : entities_) {
            bridge_->destroy(ent.local_handle);
        }
    }
}

void NetworkEntityManager::set_bridge(INetworkEntityBridge* bridge) {
    bridge_ = bridge;
}

void NetworkEntityManager::set_max_entities(std::size_t max) noexcept {
    max_entities_ = max;
}

// ── Server API ──────────────────────────────────────────────────────────────

NetworkId NetworkEntityManager::spawn(PrefabId prefab, const std::vector<std::uint8_t>& init_data,
                                      NetworkManager* net) {
    if (max_entities_ > 0 && entities_.size() >= max_entities_) {
        return 0;
    }

    NetworkId id = next_id_++;

    Entity ent;
    ent.net_id = id;
    ent.prefab_id = prefab;
    if (bridge_) {
        ent.local_handle = bridge_->spawn(prefab, id, init_data);
    }

    entities_[id] = ent;

    if (net) {
        broadcast_spawn(ent, init_data, *net);
    }

    return id;
}

void NetworkEntityManager::destroy(NetworkId id, NetworkManager* net) {
    auto it = entities_.find(id);
    if (it == entities_.end())
        return;

    if (bridge_) {
        bridge_->destroy(it->second.local_handle);
    }

    entities_.erase(it);

    if (net) {
        broadcast_destroy(id, *net);
    }
}

void NetworkEntityManager::set_owner(NetworkId id, ClientId owner) {
    auto it = entities_.find(id);
    if (it != entities_.end()) {
        it->second.owner_id = owner;
    }
}

// ── Broadcasting ────────────────────────────────────────────────────────────

void NetworkEntityManager::broadcast_spawn(const Entity& ent, const std::vector<std::uint8_t>& init_data,
                                           NetworkManager& net) {
    // System message: [0xCA][0xFE][0x10][net_id 8][prefab 4][init_len 2][init_data...]
    std::size_t payload_size = 8 + 4 + 2 + init_data.size();
    std::vector<std::uint8_t> msg(3 + payload_size);
    msg[0] = 0xCA;
    msg[1] = 0xFE;
    msg[2] = 0x10; // EntitySpawn
    write_u64_be(msg.data() + 3, ent.net_id);
    write_u32_be(msg.data() + 11, ent.prefab_id);
    write_u16_be(msg.data() + 15, static_cast<std::uint16_t>(init_data.size()));
    if (!init_data.empty()) {
        std::memcpy(msg.data() + 17, init_data.data(), init_data.size());
    }

    // Interest management (spatial culling) is handled by NetworkReplicationManager
    // during snapshot building. Entity spawn messages go to all connected clients.
    net.broadcast(msg.data(), msg.size(), transport::PacketReliability::ReliableOrdered);
}

void NetworkEntityManager::broadcast_destroy(NetworkId id, NetworkManager& net) {
    // System message: [0xCA][0xFE][0x11][net_id 8]
    std::array<std::uint8_t, 11> msg{};
    msg[0] = 0xCA;
    msg[1] = 0xFE;
    msg[2] = 0x11; // EntityDestroy
    write_u64_be(msg.data() + 3, id);
    net.broadcast(msg.data(), msg.size(), transport::PacketReliability::ReliableOrdered);
}

// ── Late-joiner catch-up ────────────────────────────────────────────────────

void NetworkEntityManager::send_full_state_to_client(ClientId client, NetworkManager& net) {
    // For simplicity, send one spawn message per entity.
    for (const auto& [id, ent] : entities_) {
        std::size_t payload_size = 8 + 4 + 2; // no init_data
        std::vector<std::uint8_t> msg(3 + payload_size);
        msg[0] = 0xCA;
        msg[1] = 0xFE;
        msg[2] = 0x10; // EntitySpawn
        write_u64_be(msg.data() + 3, ent.net_id);
        write_u32_be(msg.data() + 11, ent.prefab_id);
        write_u16_be(msg.data() + 15, 0);
        net.send(client, msg.data(), msg.size(), transport::PacketReliability::ReliableOrdered);
    }
}

// ── Message handlers (client-side) ──────────────────────────────────────────

void NetworkEntityManager::on_receive_spawn(ClientId /*sender*/, const std::uint8_t* data, std::size_t len) {
    if (len < 14)
        return;

    NetworkId net_id = read_u64_be(data);
    PrefabId prefab = read_u32_be(data + 8);
    std::uint16_t init_len = read_u16_be(data + 12);

    if (len < 14 + init_len)
        return;

    std::vector<std::uint8_t> init_data;
    if (init_len > 0) {
        init_data.assign(data + 14, data + 14 + init_len);
    }

    // If entity already exists, ignore (duplicate spawn)
    if (entities_.find(net_id) != entities_.end())
        return;

    if (max_entities_ > 0 && entities_.size() >= max_entities_) {
        return;
    }

    Entity ent;
    ent.net_id = net_id;
    ent.prefab_id = prefab;
    if (bridge_) {
        ent.local_handle = bridge_->spawn(prefab, net_id, init_data);
    }
    entities_[net_id] = ent;
}

void NetworkEntityManager::on_receive_destroy(ClientId /*sender*/, const std::uint8_t* data, std::size_t len) {
    if (len < 8)
        return;
    NetworkId net_id = read_u64_be(data);

    auto it = entities_.find(net_id);
    if (it == entities_.end())
        return;

    if (bridge_) {
        bridge_->destroy(it->second.local_handle);
    }
    entities_.erase(it);
}

void NetworkEntityManager::on_receive_set_owner(ClientId /*sender*/, const std::uint8_t* data, std::size_t len) {
    if (len < 16)
        return;
    NetworkId net_id = read_u64_be(data);
    ClientId owner = read_u64_be(data + 8);

    auto it = entities_.find(net_id);
    if (it != entities_.end()) {
        it->second.owner_id = owner;
    }
}

void NetworkEntityManager::on_receive_full_state(ClientId /*sender*/, const std::uint8_t* data, std::size_t len) {
    // Full state is sent as individual spawn messages for now.
    // This handler is a placeholder for a batched format.
    (void)data;
    (void)len;
}

// ── Queries ─────────────────────────────────────────────────────────────────

bool NetworkEntityManager::exists(NetworkId id) const noexcept {
    return entities_.find(id) != entities_.end();
}

EntityHandle NetworkEntityManager::local_handle(NetworkId id) const noexcept {
    auto it = entities_.find(id);
    return it != entities_.end() ? it->second.local_handle : EntityHandle{0};
}

PrefabId NetworkEntityManager::prefab(NetworkId id) const noexcept {
    auto it = entities_.find(id);
    return it != entities_.end() ? it->second.prefab_id : PrefabId{0};
}

ClientId NetworkEntityManager::owner(NetworkId id) const noexcept {
    auto it = entities_.find(id);
    return it != entities_.end() ? it->second.owner_id : ClientId{0};
}

std::size_t NetworkEntityManager::entity_count() const noexcept {
    return entities_.size();
}

void NetworkEntityManager::for_each_entity(std::function<void(NetworkId, PrefabId, EntityHandle, ClientId)> cb) const {
    for (const auto& [id, ent] : entities_) {
        cb(id, ent.prefab_id, ent.local_handle, ent.owner_id);
    }
}

} // namespace systems::leal::campello_net

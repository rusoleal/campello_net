#pragma once

#include "campello_net/network_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace systems::leal::campello_net {

using NetworkId = std::uint64_t;
using PrefabId = std::uint32_t;

/// Opaque handle to a local entity. The ECS layer decides what this means.
using EntityHandle = std::uintptr_t;

/// Bridge interface between the network layer and the ECS layer.
/// campello_core implements this; campello_net remains ECS-agnostic.
class INetworkEntityBridge {
public:
    virtual ~INetworkEntityBridge() = default;

    /// Spawn a local entity from a prefab. Called on server and clients.
    /// @param prefab    Prefab identifier (meaning defined by ECS layer).
    /// @param net_id    Globally unique network identifier for this entity.
    /// @param init_data Optional initialization blob (e.g. transform, name).
    /// @return Opaque handle to the created local entity.
    virtual EntityHandle spawn(PrefabId prefab, NetworkId net_id, const std::vector<std::uint8_t>& init_data) = 0;

    /// Destroy a local entity. Called on server and clients.
    virtual void destroy(EntityHandle handle) = 0;
};

/// Server-authoritative manager for networked entity lifecycle.
///
/// Responsibilities:
///   - Allocate / recycle NetworkIds
///   - Track which entities exist and who owns them
///   - Send spawn / destroy messages via NetworkManager
///   - Provide late-joiner catch-up (send full state to new clients)
class NetworkEntityManager {
public:
    NetworkEntityManager() = default;
    ~NetworkEntityManager();

    NetworkEntityManager(const NetworkEntityManager&) = delete;
    NetworkEntityManager& operator=(const NetworkEntityManager&) = delete;

    // ── Bridge ───────────────────────────────────────────────────────────────

    /// Set the bridge that creates/destroys local entities. Must be set before spawn().
    void set_bridge(INetworkEntityBridge* bridge);

    /// Hard cap on total entities (0 = unlimited). Spawns beyond the limit return 0.
    void set_max_entities(std::size_t max) noexcept;

    // ── Server API ───────────────────────────────────────────────────────────

    /// Spawn a new networked entity. Returns the assigned NetworkId.
    /// If `net` is non-null, broadcasts spawn to all connected clients.
    /// Returns 0 if the entity limit has been reached.
    NetworkId spawn(PrefabId prefab, const std::vector<std::uint8_t>& init_data = {}, NetworkManager* net = nullptr);

    /// Destroy an entity by NetworkId.
    /// If `net` is non-null, broadcasts destroy to all connected clients.
    void destroy(NetworkId id, NetworkManager* net = nullptr);

    /// Change the owner of an entity (0 = server-owned).
    void set_owner(NetworkId id, ClientId owner);

    /// Send the full entity state to a specific client (late-joiner catch-up).
    void send_full_state_to_client(ClientId client, NetworkManager& net);

    // ── Message handling (called by NetworkManager) ──────────────────────────

    /// Parse and apply an EntitySpawn system message (client-side).
    void on_receive_spawn(ClientId sender, const std::uint8_t* data, std::size_t len);

    /// Parse and apply an EntityDestroy system message (client-side).
    void on_receive_destroy(ClientId sender, const std::uint8_t* data, std::size_t len);

    /// Parse and apply an EntitySetOwner system message (client-side).
    void on_receive_set_owner(ClientId sender, const std::uint8_t* data, std::size_t len);

    /// Placeholder for batched full-state format. Currently a no-op.
    void on_receive_full_state(ClientId sender, const std::uint8_t* data, std::size_t len);

    // ── Queries ──────────────────────────────────────────────────────────────

    /// True if the given NetworkId is currently tracked.
    [[nodiscard]] bool exists(NetworkId id) const noexcept;

    /// Local ECS handle for the given NetworkId (0 if not found).
    [[nodiscard]] EntityHandle local_handle(NetworkId id) const noexcept;

    /// Prefab identifier for the given NetworkId (0 if not found).
    [[nodiscard]] PrefabId prefab(NetworkId id) const noexcept;

    /// Current owner of the given NetworkId (0 = server-owned).
    [[nodiscard]] ClientId owner(NetworkId id) const noexcept;

    /// Total number of tracked entities.
    [[nodiscard]] std::size_t entity_count() const noexcept;

    /// Iterate all tracked entities. Callback receives (net_id, prefab_id, local_handle, owner_id).
    void for_each_entity(std::function<void(NetworkId, PrefabId, EntityHandle, ClientId)> cb) const;

private:
    struct Entity {
        NetworkId net_id = 0;
        PrefabId prefab_id = 0;
        EntityHandle local_handle = 0;
        ClientId owner_id = 0;
    };

    std::unordered_map<NetworkId, Entity> entities_;
    INetworkEntityBridge* bridge_ = nullptr;
    NetworkId next_id_ = 1;
    std::size_t max_entities_ = 0;

    void broadcast_spawn(const Entity& ent, const std::vector<std::uint8_t>& init_data, NetworkManager& net);
    void broadcast_destroy(NetworkId id, NetworkManager& net);

    // Serialization helpers (big-endian on wire)
    static void write_u64_be(std::uint8_t* dst, std::uint64_t value) noexcept;
    static void write_u32_be(std::uint8_t* dst, std::uint32_t value) noexcept;
    static void write_u16_be(std::uint8_t* dst, std::uint16_t value) noexcept;
    static std::uint64_t read_u64_be(const std::uint8_t* src) noexcept;
    static std::uint32_t read_u32_be(const std::uint8_t* src) noexcept;
    static std::uint16_t read_u16_be(const std::uint8_t* src) noexcept;
};

} // namespace systems::leal::campello_net

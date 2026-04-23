#pragma once

#include "campello_net/network_entity.hpp"
#include "campello_net/network_replication.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace systems::leal::campello_net {

/// Default spatial partitioning interest manager.
///
/// Organises entities into a uniform 3D grid. Queries are accelerated by
/// only inspecting cells that overlap the query sphere. This is the
/// recommended default for open-world / MMO-style games with many static
/// or dynamic entities.
///
/// Usage:
///   SpatialInterestManager spatial(100.0f); // 100 m cells
///   spatial.register_entity(id, x, y, z);
///   spatial.register_client(client_id, x, y, z);
///   repl.set_interest_filter(spatial.get_filter());
class SpatialInterestManager {
public:
    explicit SpatialInterestManager(float cell_size = 100.0f);

    // ── Configuration ────────────────────────────────────────────────────────

    void set_cell_size(float cell_size) noexcept;
    void set_default_relevancy_radius(float radius) noexcept;

    /// Hard cap on how many entities a single client may receive (0 = unlimited).
    void set_max_entities_per_client(std::size_t max) noexcept;

    // ── Entity tracking ──────────────────────────────────────────────────────

    void register_entity(NetworkId net_id, float x, float y, float z = 0.0f);
    void update_entity(NetworkId net_id, float x, float y, float z = 0.0f);
    void remove_entity(NetworkId net_id);

    /// Override the default relevancy radius for a specific entity (0 = use default).
    void set_entity_relevancy_radius(NetworkId net_id, float radius) noexcept;

    // ── Client tracking ──────────────────────────────────────────────────────

    void register_client(ClientId client_id, float x, float y, float z = 0.0f);
    void update_client(ClientId client_id, float x, float y, float z = 0.0f);
    void remove_client(ClientId client_id);

    /// Override the default view radius for a specific client (0 = use default).
    void set_client_view_radius(ClientId client_id, float radius) noexcept;

    // ── Queries ──────────────────────────────────────────────────────────────

    /// Check whether @p entity should be replicated to @p client.
    [[nodiscard]] bool is_relevant(NetworkId entity, ClientId client) const;

    /// Return all entities within @p radius of the given point.
    void query_sphere(float x, float y, float z, float radius, std::vector<NetworkId>& out) const;

    /// Return all entities visible to @p client.
    void query_visible(ClientId client_id, std::vector<NetworkId>& out) const;

    /// Produce a lambda suitable for NetworkReplicationManager::set_interest_filter().
    [[nodiscard]] NetworkReplicationManager::InterestFilter get_filter();

    void clear();

    [[nodiscard]] std::size_t entity_count() const noexcept;
    [[nodiscard]] std::size_t client_count() const noexcept;

private:
    struct Vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct CellCoord {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        bool operator==(const CellCoord&) const noexcept = default;
    };

    struct CellCoordHash {
        std::size_t operator()(CellCoord c) const noexcept;
    };

    struct EntityEntry {
        NetworkId id = 0;
        Vec3 pos{};
        CellCoord cell{};
        float radius = 0.0f; // 0 = use default
    };

    struct ClientEntry {
        ClientId id = 0;
        Vec3 pos{};
        float radius = 0.0f; // 0 = use default
    };

    float cell_size_ = 100.0f;
    float default_relevancy_radius_ = 500.0f;
    std::size_t max_entities_per_client_ = 0;

    std::unordered_map<NetworkId, EntityEntry> entities_;
    std::unordered_map<ClientId, ClientEntry> clients_;
    std::unordered_map<CellCoord, std::vector<NetworkId>, CellCoordHash> cells_;

    void add_to_cell(const CellCoord& cell, NetworkId id);
    void remove_from_cell(const CellCoord& cell, NetworkId id);

    [[nodiscard]] CellCoord world_to_cell(float x, float y, float z) const noexcept;
    [[nodiscard]] float get_relevancy_radius(NetworkId entity, ClientId client) const noexcept;
};

} // namespace systems::leal::campello_net

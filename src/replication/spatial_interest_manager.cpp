#include "campello_net/replication/spatial_interest_manager.hpp"

#include <algorithm>
#include <cmath>

namespace systems::leal::campello_net {

// ── CellCoord hash ──────────────────────────────────────────────────────────

std::size_t SpatialInterestManager::CellCoordHash::operator()(CellCoord c) const noexcept {
    std::size_t h = std::hash<int64_t>{}(c.x);
    h ^= std::hash<int64_t>{}(c.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(c.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

// ── Construction / config ───────────────────────────────────────────────────

SpatialInterestManager::SpatialInterestManager(float cell_size) : cell_size_(cell_size) {}

void SpatialInterestManager::set_cell_size(float cell_size) noexcept {
    cell_size_ = cell_size;
}

void SpatialInterestManager::set_default_relevancy_radius(float radius) noexcept {
    default_relevancy_radius_ = radius;
}

void SpatialInterestManager::set_max_entities_per_client(std::size_t max) noexcept {
    max_entities_per_client_ = max;
}

// ── Entity tracking ─────────────────────────────────────────────────────────

void SpatialInterestManager::register_entity(NetworkId net_id, float x, float y, float z) {
    auto it = entities_.find(net_id);
    if (it != entities_.end()) {
        update_entity(net_id, x, y, z);
        return;
    }

    EntityEntry ent;
    ent.id = net_id;
    ent.pos = {x, y, z};
    ent.cell = world_to_cell(x, y, z);

    entities_.emplace(net_id, std::move(ent));
    add_to_cell(ent.cell, net_id);
}

void SpatialInterestManager::update_entity(NetworkId net_id, float x, float y, float z) {
    auto it = entities_.find(net_id);
    if (it == entities_.end())
        return;

    auto& ent = it->second;
    if (ent.pos.x == x && ent.pos.y == y && ent.pos.z == z)
        return;

    const CellCoord old_cell = ent.cell;
    ent.pos = {x, y, z};
    ent.cell = world_to_cell(x, y, z);

    if (!(old_cell == ent.cell)) {
        remove_from_cell(old_cell, net_id);
        add_to_cell(ent.cell, net_id);
    }
}

void SpatialInterestManager::remove_entity(NetworkId net_id) {
    auto it = entities_.find(net_id);
    if (it == entities_.end())
        return;

    remove_from_cell(it->second.cell, net_id);
    entities_.erase(it);
}

void SpatialInterestManager::set_entity_relevancy_radius(NetworkId net_id, float radius) noexcept {
    auto it = entities_.find(net_id);
    if (it != entities_.end()) {
        it->second.radius = radius;
    }
}

// ── Client tracking ─────────────────────────────────────────────────────────

void SpatialInterestManager::register_client(ClientId client_id, float x, float y, float z) {
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        update_client(client_id, x, y, z);
        return;
    }

    ClientEntry ent;
    ent.id = client_id;
    ent.pos = {x, y, z};
    clients_.emplace(client_id, std::move(ent));
}

void SpatialInterestManager::update_client(ClientId client_id, float x, float y, float z) {
    auto it = clients_.find(client_id);
    if (it == clients_.end())
        return;

    it->second.pos = {x, y, z};
}

void SpatialInterestManager::remove_client(ClientId client_id) {
    clients_.erase(client_id);
}

void SpatialInterestManager::set_client_view_radius(ClientId client_id, float radius) noexcept {
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        it->second.radius = radius;
    }
}

// ── Queries ─────────────────────────────────────────────────────────────────

bool SpatialInterestManager::is_relevant(NetworkId entity, ClientId client) const {
    auto eit = entities_.find(entity);
    auto cit = clients_.find(client);
    if (eit == entities_.end() || cit == clients_.end())
        return false;

    const Vec3& ep = eit->second.pos;
    const Vec3& cp = cit->second.pos;

    const float dx = ep.x - cp.x;
    const float dy = ep.y - cp.y;
    const float dz = ep.z - cp.z;
    const float dist_sq = dx * dx + dy * dy + dz * dz;

    const float radius = get_relevancy_radius(entity, client);
    return dist_sq <= radius * radius;
}

void SpatialInterestManager::query_sphere(float x, float y, float z, float radius, std::vector<NetworkId>& out) const {
    if (radius <= 0.0f)
        return;

    const CellCoord min_cell = world_to_cell(x - radius, y - radius, z - radius);
    const CellCoord max_cell = world_to_cell(x + radius, y + radius, z + radius);
    const float radius_sq = radius * radius;

    for (int64_t cx = min_cell.x; cx <= max_cell.x; ++cx) {
        for (int64_t cy = min_cell.y; cy <= max_cell.y; ++cy) {
            for (int64_t cz = min_cell.z; cz <= max_cell.z; ++cz) {
                auto it = cells_.find({cx, cy, cz});
                if (it == cells_.end())
                    continue;

                for (NetworkId id : it->second) {
                    auto eit = entities_.find(id);
                    if (eit == entities_.end())
                        continue;

                    const Vec3& pos = eit->second.pos;
                    const float dx = pos.x - x;
                    const float dy = pos.y - y;
                    const float dz = pos.z - z;
                    if (dx * dx + dy * dy + dz * dz <= radius_sq) {
                        out.push_back(id);
                    }
                }
            }
        }
    }
}

void SpatialInterestManager::query_visible(ClientId client_id, std::vector<NetworkId>& out) const {
    auto cit = clients_.find(client_id);
    if (cit == clients_.end())
        return;

    const Vec3& cp = cit->second.pos;
    const float radius = cit->second.radius > 0.0f ? cit->second.radius : default_relevancy_radius_;

    query_sphere(cp.x, cp.y, cp.z, radius, out);

    if (max_entities_per_client_ > 0 && out.size() > max_entities_per_client_) {
        // Simple distance sort and truncation
        std::sort(out.begin(), out.end(), [&](NetworkId a, NetworkId b) {
            auto ait = entities_.find(a);
            auto bit = entities_.find(b);
            if (ait == entities_.end() || bit == entities_.end())
                return ait != entities_.end();

            const Vec3& ap = ait->second.pos;
            const Vec3& bp = bit->second.pos;

            const float adx = ap.x - cp.x;
            const float ady = ap.y - cp.y;
            const float adz = ap.z - cp.z;
            const float bdx = bp.x - cp.x;
            const float bdy = bp.y - cp.y;
            const float bdz = bp.z - cp.z;

            return (adx * adx + ady * ady + adz * adz) < (bdx * bdx + bdy * bdy + bdz * bdz);
        });
        out.resize(max_entities_per_client_);
    }
}

NetworkReplicationManager::InterestFilter SpatialInterestManager::get_filter() {
    return [this](NetworkId entity, ClientId client) {
        return is_relevant(entity, client);
    };
}

void SpatialInterestManager::clear() {
    entities_.clear();
    clients_.clear();
    cells_.clear();
}

std::size_t SpatialInterestManager::entity_count() const noexcept {
    return entities_.size();
}

std::size_t SpatialInterestManager::client_count() const noexcept {
    return clients_.size();
}

// ── Helpers ─────────────────────────────────────────────────────────────────

SpatialInterestManager::CellCoord SpatialInterestManager::world_to_cell(float x, float y, float z) const noexcept {
    return {
        static_cast<int64_t>(std::floor(x / cell_size_)),
        static_cast<int64_t>(std::floor(y / cell_size_)),
        static_cast<int64_t>(std::floor(z / cell_size_)),
    };
}

float SpatialInterestManager::get_relevancy_radius(NetworkId entity, ClientId client) const noexcept {
    float radius = default_relevancy_radius_;

    auto eit = entities_.find(entity);
    if (eit != entities_.end() && eit->second.radius > 0.0f) {
        radius = eit->second.radius;
    }

    auto cit = clients_.find(client);
    if (cit != clients_.end() && cit->second.radius > 0.0f) {
        radius = cit->second.radius;
    }

    return radius;
}

void SpatialInterestManager::add_to_cell(const CellCoord& cell, NetworkId id) {
    cells_[cell].push_back(id);
}

void SpatialInterestManager::remove_from_cell(const CellCoord& cell, NetworkId id) {
    auto it = cells_.find(cell);
    if (it == cells_.end())
        return;

    auto& vec = it->second;
    auto vit = std::find(vec.begin(), vec.end(), id);
    if (vit != vec.end()) {
        vec.erase(vit);
        if (vec.empty()) {
            cells_.erase(it);
        }
    }
}

} // namespace systems::leal::campello_net

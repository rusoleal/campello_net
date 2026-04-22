#include "campello_net/network_replication.hpp"

#include <cstring>
#include <iostream>
#include <span>

namespace systems::leal::campello_net {

// Helper: fix up spans after a vector reallocation invalidates previous data().
static void fixup_spans_after_realloc(std::vector<std::uint8_t>& blob,
                                      std::vector<EntitySnapshot>& snaps,
                                      const std::uint8_t* old_data) {
    if (blob.data() == old_data)
        return;
    for (auto& ent : snaps) {
        if (ent.data.empty())
            continue;
        std::ptrdiff_t rel = ent.data.data() - old_data;
        ent.data = std::span<const std::uint8_t>(blob.data() + static_cast<std::size_t>(rel), ent.data.size());
    }
}

// ════════════════════════════════════════════════════════════════════════════
// SnapshotHistory
// ════════════════════════════════════════════════════════════════════════════

void SnapshotHistory::store(std::uint16_t snapshot_id, std::span<const EntitySnapshot> entities) {
    head_ = (head_ + 1) % MAX_SNAPSHOTS;
    Snapshot& slot = snapshots_[head_];
    slot.id = snapshot_id;

    // Flatten all entity data into a single blob to avoid per-entity allocations.
    std::size_t total_bytes = 0;
    for (const auto& ent : entities) {
        total_bytes += ent.data.size();
    }
    slot.blob.clear();
    slot.blob.reserve(total_bytes);
    slot.entities.clear();
    slot.entities.reserve(entities.size());

    for (const auto& ent : entities) {
        std::size_t offset = slot.blob.size();
        slot.blob.insert(slot.blob.end(), ent.data.begin(), ent.data.end());
        slot.entities.push_back({ent.id, std::span<const std::uint8_t>(slot.blob.data() + offset, ent.data.size())});
    }

    if (count_ < MAX_SNAPSHOTS)
        ++count_;
}

const std::vector<EntitySnapshot>* SnapshotHistory::retrieve(std::uint16_t snapshot_id) const {
    if (count_ == 0)
        return nullptr;

    // O(1) lookup: snapshot IDs are contiguous (increment by 1 per tick).
    // uint16_t subtraction handles wrap-around automatically.
    std::uint16_t head_id = snapshots_[head_].id;
    std::uint16_t delta = head_id - snapshot_id;
    if (delta >= count_)
        return nullptr; // too old or not yet stored

    std::size_t idx = (head_ + MAX_SNAPSHOTS - delta) % MAX_SNAPSHOTS;
    return &snapshots_[idx].entities;
}

// ════════════════════════════════════════════════════════════════════════════
// NetworkReplicationManager
// ════════════════════════════════════════════════════════════════════════════

// ── Configuration ───────────────────────────────────────────────────────────

void NetworkReplicationManager::set_bridge(INetworkReplicationBridge* bridge) {
    bridge_ = bridge;
}

void NetworkReplicationManager::set_entity_manager(NetworkEntityManager* entity_mgr) {
    entity_mgr_ = entity_mgr;
}

void NetworkReplicationManager::set_full_sync_interval(std::uint16_t interval) noexcept {
    full_sync_interval_ = interval;
}

void NetworkReplicationManager::set_tick_rate(float hz) noexcept {
    tick_rate_ = hz;
}

void NetworkReplicationManager::set_max_baseline_age(std::uint16_t age) noexcept {
    max_baseline_age_ = age;
}

void NetworkReplicationManager::set_interest_filter(InterestFilter filter) {
    interest_filter_ = std::move(filter);
}

// ── Dirty tracking ──────────────────────────────────────────────────────────

void NetworkReplicationManager::mark_dirty(NetworkId net_id) {
    dirty_entities_.insert(net_id);
}

// ── Server tick ─────────────────────────────────────────────────────────────

void NetworkReplicationManager::server_tick(float delta_time, NetworkManager& net) {
    if (net.mode() != NetworkManager::Mode::Server && net.mode() != NetworkManager::Mode::Host) {
        return;
    }
    if (!bridge_)
        return;

    accumulator_ += delta_time;
    const float target_dt = 1.0f / tick_rate_;

    while (accumulator_ >= target_dt) {
        accumulator_ -= target_dt;
        ++snapshot_id_;
        build_and_send_snapshot(net, false);
    }
}

// ── Snapshot ack handling ───────────────────────────────────────────────────

void NetworkReplicationManager::on_snapshot_ack(ClientId client, std::uint16_t snapshot_id) {
    auto& state = client_states_[client];
    // Only accept newer acks (handles uint16_t wrap-around).
    if (!state.has_ack || static_cast<std::int16_t>(snapshot_id - state.last_acked_snapshot) > 0) {
        state.last_acked_snapshot = snapshot_id;
        state.has_ack = true;
    }
}

void NetworkReplicationManager::on_client_connected(ClientId client) {
    // Ensure the client has a replication state entry.
    client_states_[client] = ClientReplicationState{};
}

void NetworkReplicationManager::on_client_disconnected(ClientId client) {
    client_states_.erase(client);
}

// ── Snapshot building ───────────────────────────────────────────────────────

void NetworkReplicationManager::build_and_send_snapshot(NetworkManager& net, bool /*full_sync*/) {
    if (client_states_.empty()) {
        dirty_entities_.clear();
        return;
    }

    const bool use_interest = (interest_filter_ != nullptr);

    // Determine whether any client needs a full sync this tick.
    bool force_full_sync = (full_sync_interval_ > 0) && (snapshot_id_ % full_sync_interval_ == 0);
    bool any_needs_full = force_full_sync;
    if (!any_needs_full) {
        for (auto& [client_id, state] : client_states_) {
            if (!net.is_client_connected(client_id))
                continue;
            if (!state.has_ack) {
                any_needs_full = true;
                break;
            }
        }
    }

    // Build the current snapshot into a flat blob with non-owning spans.
    // Reuse scratch buffers so no allocations occur after warm-up.
    scratch_stream_.reset();
    scratch_blob_.clear();
    scratch_snap_.clear();

    const bool build_full = use_interest || (any_needs_full && entity_mgr_);
    if (build_full && entity_mgr_) {
        entity_mgr_->for_each_entity([&](NetworkId id, PrefabId, EntityHandle, ClientId) {
            scratch_stream_.reset();
            if (!bridge_->serialize_entity(id, scratch_stream_))
                return;
            auto span = scratch_stream_.span();
            std::size_t offset = scratch_blob_.size();
            std::uint8_t* old_data = scratch_blob_.data();
            scratch_blob_.insert(scratch_blob_.end(), span.begin(), span.end());
            fixup_spans_after_realloc(scratch_blob_, scratch_snap_, old_data);
            scratch_snap_.push_back({id, std::span<const std::uint8_t>(scratch_blob_.data() + offset, span.size())});
        });
    } else {
        scratch_snap_.reserve(dirty_entities_.size());
        for (NetworkId id : dirty_entities_) {
            scratch_stream_.reset();
            if (!bridge_->serialize_entity(id, scratch_stream_))
                continue;
            auto span = scratch_stream_.span();
            std::size_t offset = scratch_blob_.size();
            std::uint8_t* old_data = scratch_blob_.data();
            scratch_blob_.insert(scratch_blob_.end(), span.begin(), span.end());
            fixup_spans_after_realloc(scratch_blob_, scratch_snap_, old_data);
            scratch_snap_.push_back({id, std::span<const std::uint8_t>(scratch_blob_.data() + offset, span.size())});
        }
    }

    // Store in history so future deltas can reference it.
    snapshot_history_.store(snapshot_id_, scratch_snap_);

    // Send to every connected client.
    for (auto& [client_id, state] : client_states_) {
        if (!net.is_client_connected(client_id))
            continue;

        bool send_full = force_full_sync || !state.has_ack;

        if (!send_full && state.has_ack) {
            const std::vector<EntitySnapshot>* baseline = snapshot_history_.retrieve(state.last_acked_snapshot);

            std::uint16_t age = snapshot_id_ - state.last_acked_snapshot;
            if (!baseline || age > max_baseline_age_) {
                send_full = true;
            } else {
                // Build delta: only entities whose bytes differ from baseline.
                // Use a linear scan instead of an unordered_map to avoid allocation.
                scratch_delta_.clear();
                scratch_delta_.reserve(scratch_snap_.size());
                for (const auto& cur : scratch_snap_) {
                    // Interest filtering.
                    if (use_interest) {
                        if (!interest_filter_(cur.id, client_id)) {
                            state.visible_entities.erase(cur.id);
                            continue;
                        }
                        bool newly_visible = state.visible_entities.find(cur.id) == state.visible_entities.end();
                        state.visible_entities.insert(cur.id);
                        if (newly_visible) {
                            scratch_delta_.push_back(cur);
                            continue;
                        }
                    }

                    bool changed = true;
                    for (const auto& base : *baseline) {
                        if (base.id == cur.id) {
                            changed = !std::equal(base.data.begin(), base.data.end(), cur.data.begin(), cur.data.end());
                            break;
                        }
                    }
                    if (changed) {
                        scratch_delta_.push_back(cur);
                    }
                }
                send_snapshot_to_client(client_id, scratch_delta_, net);
                continue;
            }
        }

        if (use_interest) {
            // Full sync with interest filter: still need to track visibility.
            scratch_filtered_.clear();
            scratch_filtered_.reserve(scratch_snap_.size());
            for (const auto& ent : scratch_snap_) {
                if (interest_filter_(ent.id, client_id)) {
                    state.visible_entities.insert(ent.id);
                    scratch_filtered_.push_back(ent);
                } else {
                    state.visible_entities.erase(ent.id);
                }
            }
            send_snapshot_to_client(client_id, scratch_filtered_, net);
        } else {
            send_snapshot_to_client(client_id, scratch_snap_, net);
        }
    }

    // Clean up dirty set.
    dirty_entities_.clear();
}

void NetworkReplicationManager::send_snapshot_to_client(ClientId client, const std::vector<EntitySnapshot>& entities,
                                                        NetworkManager& net) {
    if (entities.empty())
        return;

    // Reuse packet buffer to avoid per-send allocations.
    static thread_local std::vector<std::uint8_t> packet;
    static thread_local std::vector<std::uint8_t> sys_msg;

    // Header: [snapshot_id 2][num_entities 2]
    packet.clear();
    packet.reserve(4 + entities.size() * 18); // rough estimate
    packet.push_back(static_cast<std::uint8_t>(snapshot_id_ >> 8));
    packet.push_back(static_cast<std::uint8_t>(snapshot_id_ & 0xFF));
    packet.push_back(static_cast<std::uint8_t>((entities.size() >> 8) & 0xFF));
    packet.push_back(static_cast<std::uint8_t>(entities.size() & 0xFF));

    for (const auto& ent : entities) {
        std::uint16_t data_len = static_cast<std::uint16_t>(ent.data.size());
        std::size_t offset = packet.size();
        packet.resize(offset + 10 + data_len);

        packet[offset + 0] = static_cast<std::uint8_t>(ent.id >> 56);
        packet[offset + 1] = static_cast<std::uint8_t>(ent.id >> 48);
        packet[offset + 2] = static_cast<std::uint8_t>(ent.id >> 40);
        packet[offset + 3] = static_cast<std::uint8_t>(ent.id >> 32);
        packet[offset + 4] = static_cast<std::uint8_t>(ent.id >> 24);
        packet[offset + 5] = static_cast<std::uint8_t>(ent.id >> 16);
        packet[offset + 6] = static_cast<std::uint8_t>(ent.id >> 8);
        packet[offset + 7] = static_cast<std::uint8_t>(ent.id);

        packet[offset + 8] = static_cast<std::uint8_t>(data_len >> 8);
        packet[offset + 9] = static_cast<std::uint8_t>(data_len & 0xFF);

        if (data_len > 0) {
            std::memcpy(packet.data() + offset + 10, ent.data.data(), data_len);
        }
    }

    // Wrap in system message protocol
    sys_msg.clear();
    sys_msg.reserve(3 + packet.size());
    sys_msg.push_back(0xCA);
    sys_msg.push_back(0xFE);
    sys_msg.push_back(0x20); // DeltaState
    sys_msg.insert(sys_msg.end(), packet.begin(), packet.end());

    net.send(client, sys_msg.data(), sys_msg.size(), transport::PacketReliability::Unreliable);
}

// ── Client delta application ────────────────────────────────────────────────

void NetworkReplicationManager::on_receive_delta(const std::uint8_t* data, std::size_t len) {
    if (len < 4)
        return;

    std::uint16_t snapshot_id = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    std::uint16_t num_entities = static_cast<std::uint16_t>((data[2] << 8) | data[3]);

    // Track newest received snapshot for ack and interpolation purposes.
    if (static_cast<std::int16_t>(snapshot_id - last_received_snapshot_) > 0) {
        last_received_snapshot_ = snapshot_id;
    }
    if (static_cast<std::int16_t>(snapshot_id - latest_received_snapshot_) > 0) {
        latest_received_snapshot_ = snapshot_id;
    }

    // Parse all entities into a flat blob with non-owning spans.
    // Reuse a persistent buffer so we do not allocate per entity.
    static thread_local std::vector<std::uint8_t> blob;
    static thread_local std::vector<EntitySnapshot> entities;
    blob.clear();
    entities.clear();
    entities.reserve(num_entities);

    std::size_t offset = 4;
    for (std::uint16_t i = 0; i < num_entities; ++i) {
        if (offset + 10 > len)
            break;

        NetworkId net_id = 0;
        net_id |= static_cast<NetworkId>(data[offset + 0]) << 56;
        net_id |= static_cast<NetworkId>(data[offset + 1]) << 48;
        net_id |= static_cast<NetworkId>(data[offset + 2]) << 40;
        net_id |= static_cast<NetworkId>(data[offset + 3]) << 32;
        net_id |= static_cast<NetworkId>(data[offset + 4]) << 24;
        net_id |= static_cast<NetworkId>(data[offset + 5]) << 16;
        net_id |= static_cast<NetworkId>(data[offset + 6]) << 8;
        net_id |= static_cast<NetworkId>(data[offset + 7]);

        std::uint16_t data_len = static_cast<std::uint16_t>((data[offset + 8] << 8) | data[offset + 9]);
        offset += 10;

        if (offset + data_len > len)
            break;

        std::size_t blob_offset = blob.size();
        std::uint8_t* old_data = blob.data();
        blob.insert(blob.end(), data + offset, data + offset + data_len);
        fixup_spans_after_realloc(blob, entities, old_data);
        entities.push_back({net_id, std::span<const std::uint8_t>(blob.data() + blob_offset, data_len)});
        offset += data_len;
    }

    // Prediction mode: fire callbacks immediately (game-layer reconciliation).
    if (prediction_mode_ && snapshot_cb_) {
        for (const auto& ent : entities) {
            serialization::BitStream stream(ent.data);
            snapshot_cb_(ent.id, stream);
        }
    } else if (interpolation_enabled_) {
        // Interpolation mode: buffer the snapshot for render-time sampling.
        client_snapshot_buffer_.store(snapshot_id, client_time_, entities);
    } else if (bridge_) {
        // Immediate deserialize (legacy behavior).
        for (const auto& ent : entities) {
            serialization::BitStream stream(ent.data);
            bridge_->deserialize_entity(ent.id, stream);
        }
    }
}

// ── Client ack tick ─────────────────────────────────────────────────────────

void NetworkReplicationManager::client_tick(float delta_time, NetworkManager& net) {
    if (net.mode() != NetworkManager::Mode::Client && net.mode() != NetworkManager::Mode::Host) {
        return;
    }

    client_time_ += delta_time;
    client_ack_accumulator_ += delta_time;
    const float target_dt = 1.0f / tick_rate_;

    while (client_ack_accumulator_ >= target_dt) {
        client_ack_accumulator_ -= target_dt;

        if (last_received_snapshot_ != 0) {
            std::uint8_t ack_msg[5];
            ack_msg[0] = 0xCA;
            ack_msg[1] = 0xFE;
            ack_msg[2] = 0x21; // SnapshotAck
            ack_msg[3] = static_cast<std::uint8_t>(last_received_snapshot_ >> 8);
            ack_msg[4] = static_cast<std::uint8_t>(last_received_snapshot_ & 0xFF);
            net.send(ack_msg, sizeof(ack_msg), transport::PacketReliability::Unreliable);
        }
    }
}

// ── Queries ─────────────────────────────────────────────────────────────────

std::uint16_t NetworkReplicationManager::current_snapshot_id() const noexcept {
    return snapshot_id_;
}

std::size_t NetworkReplicationManager::dirty_count() const noexcept {
    return dirty_entities_.size();
}

// ── Prediction (Phase 10) ───────────────────────────────────────────────────

void NetworkReplicationManager::set_prediction_mode(bool enabled) {
    prediction_mode_ = enabled;
}

void NetworkReplicationManager::set_snapshot_received_callback(SnapshotReceivedCallback cb) {
    snapshot_cb_ = std::move(cb);
}

void NetworkReplicationManager::set_interpolation_delay(float seconds) noexcept {
    interpolation_delay_ = seconds;
}

std::uint16_t NetworkReplicationManager::latest_received_snapshot() const noexcept {
    return latest_received_snapshot_;
}

// ── Interpolation (Phase 9) ─────────────────────────────────────────────────

void NetworkReplicationManager::set_interpolation_enabled(bool enabled) noexcept {
    interpolation_enabled_ = enabled;
}

bool NetworkReplicationManager::interpolation_enabled() const noexcept {
    return interpolation_enabled_;
}

void NetworkReplicationManager::client_interpolate(float render_time) {
    if (!bridge_ || !interpolation_enabled_ || client_snapshot_buffer_.size() < 2) {
        return;
    }

    const float target_time = render_time - interpolation_delay_;

    const ClientSnapshotBuffer::Snapshot* older = nullptr;
    const ClientSnapshotBuffer::Snapshot* newer = nullptr;
    float t = 0.0f;

    if (!client_snapshot_buffer_.find_bracketing(target_time, older, newer, t)) {
        return;
    }

    // Interpolate every entity present in the newer snapshot.
    const bool extrapolating = (target_time > newer->receive_time);
    const float extrapolation_delta = extrapolating ? (target_time - newer->receive_time) : 0.0f;

    for (const auto& ent_newer : newer->entities) {
        serialization::BitStream stream_newer(ent_newer.data);

        if (extrapolating) {
            bridge_->extrapolate_entity(ent_newer.id, stream_newer, extrapolation_delta);
        } else {
            std::span<const std::uint8_t> older_data = client_snapshot_buffer_.find_entity(*older, ent_newer.id);
            if (!older_data.empty()) {
                serialization::BitStream stream_older(older_data);
                bridge_->interpolate_entity(ent_newer.id, stream_older, stream_newer, t);
            } else {
                // Entity only exists in newer snapshot (newly visible) — snap to latest.
                bridge_->deserialize_entity(ent_newer.id, stream_newer);
            }
        }
    }
}

bool NetworkReplicationManager::query_interpolated_entity(NetworkId net_id, float render_time,
                                                          serialization::BitStream& older,
                                                          serialization::BitStream& newer,
                                                          float& t) const {
    if (!interpolation_enabled_ || client_snapshot_buffer_.size() < 2) {
        return false;
    }

    const float target_time = render_time - interpolation_delay_;

    const ClientSnapshotBuffer::Snapshot* snap_older = nullptr;
    const ClientSnapshotBuffer::Snapshot* snap_newer = nullptr;
    if (!client_snapshot_buffer_.find_bracketing(target_time, snap_older, snap_newer, t)) {
        return false;
    }

    std::span<const std::uint8_t> older_data = client_snapshot_buffer_.find_entity(*snap_older, net_id);
    std::span<const std::uint8_t> newer_data = client_snapshot_buffer_.find_entity(*snap_newer, net_id);

    if (newer_data.empty()) {
        return false;
    }

    older = serialization::BitStream(older_data);
    newer = serialization::BitStream(newer_data);
    return true;
}

// ── ClientSnapshotBuffer ────────────────────────────────────────────────────

void ClientSnapshotBuffer::store(std::uint16_t snapshot_id, float receive_time,
                                 std::span<const EntitySnapshot> entities) {
    head_ = (head_ + 1) % MAX_SNAPSHOTS;
    Snapshot& slot = snapshots_[head_];
    slot.snapshot_id = snapshot_id;
    slot.receive_time = receive_time;

    // Flatten into a single blob to avoid per-entity allocations.
    std::size_t total_bytes = 0;
    for (const auto& ent : entities) {
        total_bytes += ent.data.size();
    }
    slot.blob.clear();
    slot.blob.reserve(total_bytes);
    slot.entities.clear();
    slot.entities.reserve(entities.size());

    for (const auto& ent : entities) {
        std::size_t offset = slot.blob.size();
        slot.blob.insert(slot.blob.end(), ent.data.begin(), ent.data.end());
        slot.entities.push_back({ent.id, std::span<const std::uint8_t>(slot.blob.data() + offset, ent.data.size())});
    }

    if (count_ < MAX_SNAPSHOTS) {
        ++count_;
    }
}

bool ClientSnapshotBuffer::find_bracketing(float target_time, const Snapshot*& out_older,
                                           const Snapshot*& out_newer, float& t) const {
    if (count_ < 2) {
        return false;
    }

    const Snapshot& newest = snapshots_[head_];
    const std::size_t oldest_idx = (head_ + MAX_SNAPSHOTS - (count_ - 1)) % MAX_SNAPSHOTS;
    const Snapshot& oldest = snapshots_[oldest_idx];

    if (target_time <= oldest.receive_time) {
        // Before oldest — clamp to oldest pair.
        std::size_t second_oldest_idx = (oldest_idx + 1) % MAX_SNAPSHOTS;
        out_older = &oldest;
        out_newer = &snapshots_[second_oldest_idx];
        t = 0.0f;
        return true;
    }

    if (target_time >= newest.receive_time) {
        // After newest — clamp to newest pair.
        std::size_t second_newest_idx = (head_ + MAX_SNAPSHOTS - 1) % MAX_SNAPSHOTS;
        out_older = &snapshots_[second_newest_idx];
        out_newer = &newest;
        t = 1.0f;
        return true;
    }

    // Linear search from oldest to newest (count_ <= 128, so this is cheap).
    for (std::size_t i = 0; i < count_ - 1; ++i) {
        std::size_t idx_a = (oldest_idx + i) % MAX_SNAPSHOTS;
        std::size_t idx_b = (oldest_idx + i + 1) % MAX_SNAPSHOTS;
        const Snapshot& a = snapshots_[idx_a];
        const Snapshot& b = snapshots_[idx_b];

        if (target_time >= a.receive_time && target_time <= b.receive_time) {
            out_older = &a;
            out_newer = &b;
            const float dt = b.receive_time - a.receive_time;
            t = (dt > 0.0f) ? (target_time - a.receive_time) / dt : 0.0f;
            return true;
        }
    }

    return false; // Should not reach here with count_ >= 2.
}

std::span<const std::uint8_t> ClientSnapshotBuffer::find_entity(const Snapshot& snap,
                                                                 NetworkId net_id) const {
    for (const auto& ent : snap.entities) {
        if (ent.id == net_id) {
            return ent.data;
        }
    }
    return {};
}

void ClientSnapshotBuffer::clear() {
    for (auto& snap : snapshots_) {
        snap.blob.clear();
        snap.blob.shrink_to_fit();
        snap.entities.clear();
        snap.entities.shrink_to_fit();
    }
    count_ = 0;
    head_ = 0;
}

std::size_t ClientSnapshotBuffer::size() const noexcept {
    return count_;
}

bool ClientSnapshotBuffer::empty() const noexcept {
    return count_ == 0;
}

} // namespace systems::leal::campello_net

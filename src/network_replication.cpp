#include "campello_net/network_replication.hpp"

#include <cstring>

namespace systems::leal::campello_net {

// ════════════════════════════════════════════════════════════════════════════
// SnapshotHistory
// ════════════════════════════════════════════════════════════════════════════

void SnapshotHistory::store(std::uint16_t snapshot_id,
                            const std::vector<EntitySnapshot>& entities) {
    head_ = (head_ + 1) % MAX_SNAPSHOTS;
    snapshots_[head_].id = snapshot_id;
    snapshots_[head_].entities = entities;
    if (count_ < MAX_SNAPSHOTS) ++count_;
}

const std::vector<EntitySnapshot>* SnapshotHistory::retrieve(
    std::uint16_t snapshot_id) const {
    if (count_ == 0) return nullptr;

    // Search backwards from most recent — acks are usually for recent snaps.
    for (std::size_t i = 0; i < count_; ++i) {
        std::size_t idx = (head_ + MAX_SNAPSHOTS - i) % MAX_SNAPSHOTS;
        if (snapshots_[idx].id == snapshot_id) {
            return &snapshots_[idx].entities;
        }
    }
    return nullptr;
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
    if (!bridge_) return;

    accumulator_ += delta_time;
    const float target_dt = 1.0f / tick_rate_;

    while (accumulator_ >= target_dt) {
        accumulator_ -= target_dt;
        ++snapshot_id_;
        build_and_send_snapshot(net, false);
    }
}

// ── Snapshot ack handling ───────────────────────────────────────────────────

void NetworkReplicationManager::on_snapshot_ack(ClientId client,
                                                std::uint16_t snapshot_id) {
    auto& state = client_states_[client];
    // Only accept newer acks (handles uint16_t wrap-around).
    if (!state.has_ack ||
        static_cast<std::int16_t>(snapshot_id - state.last_acked_snapshot) > 0) {
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
    bool force_full_sync = (full_sync_interval_ > 0) &&
                           (snapshot_id_ % full_sync_interval_ == 0);
    bool any_needs_full = force_full_sync;
    if (!any_needs_full) {
        for (auto& [client_id, state] : client_states_) {
            if (!net.is_client_connected(client_id)) continue;
            if (!state.has_ack) {
                any_needs_full = true;
                break;
            }
        }
    }

    // Build the current snapshot.
    // If interest filtering is active we MUST build the full snapshot so that
    // every entity can be evaluated per-client.  Otherwise we only build the
    // dirty set unless someone needs a full sync.
    std::vector<EntitySnapshot> current_snap;
    const bool build_full = use_interest || (any_needs_full && entity_mgr_);
    if (build_full && entity_mgr_) {
        entity_mgr_->for_each_entity(
            [&current_snap, this](NetworkId id, PrefabId, EntityHandle, ClientId) {
                serialization::BitStream entity_stream;
                if (!bridge_->serialize_entity(id, entity_stream)) return;
                auto span = entity_stream.span();
                current_snap.push_back({id, std::vector<std::uint8_t>(span.begin(), span.end())});
            });
    } else {
        current_snap.reserve(dirty_entities_.size());
        for (NetworkId id : dirty_entities_) {
            serialization::BitStream entity_stream;
            if (!bridge_->serialize_entity(id, entity_stream)) continue;
            auto span = entity_stream.span();
            current_snap.push_back({id, std::vector<std::uint8_t>(span.begin(), span.end())});
        }
    }

    // Store in history so future deltas can reference it.
    snapshot_history_.store(snapshot_id_, current_snap);

    // Send to every connected client.
    for (auto& [client_id, state] : client_states_) {
        if (!net.is_client_connected(client_id)) continue;

        bool send_full = force_full_sync || !state.has_ack;
        std::vector<EntitySnapshot> to_send = current_snap;

        if (!send_full && state.has_ack) {
            const std::vector<EntitySnapshot>* baseline =
                snapshot_history_.retrieve(state.last_acked_snapshot);

            std::uint16_t age = snapshot_id_ - state.last_acked_snapshot;
            if (!baseline || age > max_baseline_age_) {
                send_full = true;
            } else {
                // Build delta: only entities whose bytes differ from baseline.
                std::unordered_map<NetworkId, const std::vector<std::uint8_t>*> baseline_map;
                baseline_map.reserve(baseline->size());
                for (const auto& e : *baseline) {
                    baseline_map[e.id] = &e.data;
                }

                to_send.clear();
                to_send.reserve(current_snap.size());
                for (const auto& cur : current_snap) {
                    // Interest filtering.
                    if (use_interest) {
                        if (!interest_filter_(cur.id, client_id)) {
                            state.visible_entities.erase(cur.id);
                            continue;
                        }
                        bool newly_visible =
                            state.visible_entities.find(cur.id) == state.visible_entities.end();
                        state.visible_entities.insert(cur.id);
                        if (newly_visible) {
                            to_send.push_back(cur);
                            continue;
                        }
                    }

                    auto it = baseline_map.find(cur.id);
                    if (it == baseline_map.end() || *it->second != cur.data) {
                        to_send.push_back(cur);
                    }
                }
            }
        } else if (use_interest) {
            // Full sync with interest filter: still need to track visibility.
            std::vector<EntitySnapshot> filtered;
            filtered.reserve(to_send.size());
            for (const auto& ent : to_send) {
                if (interest_filter_(ent.id, client_id)) {
                    state.visible_entities.insert(ent.id);
                    filtered.push_back(ent);
                } else {
                    state.visible_entities.erase(ent.id);
                }
            }
            to_send = std::move(filtered);
        }

        send_snapshot_to_client(client_id, to_send, net);
    }

    // Clean up dirty set.
    dirty_entities_.clear();
}

void NetworkReplicationManager::send_snapshot_to_client(
    ClientId client, const std::vector<EntitySnapshot>& entities, NetworkManager& net) {
    if (entities.empty()) return;

    // Header: [snapshot_id 2][num_entities 2]
    std::vector<std::uint8_t> packet(4);
    packet[0] = static_cast<std::uint8_t>(snapshot_id_ >> 8);
    packet[1] = static_cast<std::uint8_t>(snapshot_id_ & 0xFF);
    packet[2] = static_cast<std::uint8_t>((entities.size() >> 8) & 0xFF);
    packet[3] = static_cast<std::uint8_t>(entities.size() & 0xFF);

    for (const auto& ent : entities) {
        std::uint16_t data_len = static_cast<std::uint16_t>(ent.data.size());
        std::size_t offset = packet.size();
        packet.resize(offset + 8 + 2 + data_len);

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
    std::vector<std::uint8_t> sys_msg(3 + packet.size());
    sys_msg[0] = 0xCA;
    sys_msg[1] = 0xFE;
    sys_msg[2] = 0x20; // DeltaState
    std::memcpy(sys_msg.data() + 3, packet.data(), packet.size());

    net.send(client, sys_msg.data(), sys_msg.size(),
             transport::PacketReliability::Unreliable);
}

// ── Client delta application ────────────────────────────────────────────────

void NetworkReplicationManager::on_receive_delta(const std::uint8_t* data, std::size_t len) {
    if (len < 4) return;

    std::uint16_t snapshot_id = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    std::uint16_t num_entities = static_cast<std::uint16_t>((data[2] << 8) | data[3]);

    // Track newest received snapshot for ack and interpolation purposes.
    if (static_cast<std::int16_t>(snapshot_id - last_received_snapshot_) > 0) {
        last_received_snapshot_ = snapshot_id;
    }
    if (static_cast<std::int16_t>(snapshot_id - latest_received_snapshot_) > 0) {
        latest_received_snapshot_ = snapshot_id;
    }

    std::size_t offset = 4;
    for (std::uint16_t i = 0; i < num_entities; ++i) {
        if (offset + 10 > len) break;

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

        if (offset + data_len > len) break;

        serialization::BitStream stream(std::span<const std::uint8_t>(data + offset, data_len));
        if (prediction_mode_ && snapshot_cb_) {
            snapshot_cb_(net_id, stream);
        } else if (bridge_) {
            bridge_->deserialize_entity(net_id, stream);
        }
        offset += data_len;
    }
}

// ── Client ack tick ─────────────────────────────────────────────────────────

void NetworkReplicationManager::client_tick(float delta_time, NetworkManager& net) {
    if (net.mode() != NetworkManager::Mode::Client && net.mode() != NetworkManager::Mode::Host) {
        return;
    }

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

} // namespace systems::leal::campello_net

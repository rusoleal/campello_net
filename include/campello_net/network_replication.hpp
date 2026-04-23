#pragma once

#include "campello_net/network_entity.hpp"
#include "campello_net/network_manager.hpp"
#include "campello_net/serialization/bit_stream.hpp"
#include "campello_net/serialization/serializable.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace systems::leal::campello_net {

/// Bridge between the network replication layer and the ECS layer.
///
/// The ECS layer implements this interface to control what gets serialized
/// and how deserialized data is applied.
class INetworkReplicationBridge {
public:
    virtual ~INetworkReplicationBridge() = default;

    /// Server-side: serialize the current state of an entity into @p stream.
    /// Return true if there is any state worth sending.
    virtual bool serialize_entity(NetworkId net_id, serialization::BitStream& stream) = 0;

    /// Client-side: apply deserialized state to the local entity proxy.
    virtual void deserialize_entity(NetworkId net_id, serialization::BitStream& stream) = 0;

    /// Client-side: interpolate between two snapshots for smooth rendering.
    /// Default implementation falls back to the newer state.
    virtual void interpolate_entity(NetworkId net_id, serialization::BitStream& older, serialization::BitStream& newer,
                                    float t) {
        (void)net_id;
        (void)older;
        (void)t;
        deserialize_entity(net_id, newer);
    }

    /// Client-side: briefly predict forward when the newest snapshot is too old.
    /// @p delta_time is how far ahead of the newest snapshot we are rendering (seconds).
    /// Default implementation falls back to the newest state.
    virtual void extrapolate_entity(NetworkId net_id, serialization::BitStream& newest, float delta_time) {
        (void)net_id;
        (void)delta_time;
        deserialize_entity(net_id, newest);
    }
};

/// A replicated scalar variable (similar to Unity NGO's NetworkVariable).
///
/// Automatically tracks dirtiness. The owning system (ECS or game code)
/// is responsible for registering it with a NetworkReplicationManager.
template <typename T> class NetworkVariable {
public:
    NetworkVariable() = default;
    explicit NetworkVariable(const T& value) : value_(value) {}

    void set(const T& value) {
        if (value_ != value) {
            value_ = value;
            dirty_ = true;
        }
    }

    [[nodiscard]] const T& get() const {
        return value_;
    }

    [[nodiscard]] bool is_dirty() const {
        return dirty_;
    }
    void clear_dirty() {
        dirty_ = false;
    }

    void mark_dirty() {
        dirty_ = true;
    }

    void serialize(serialization::BitStream& stream) const {
        serialization::serialize(stream, value_);
    }

    bool deserialize(serialization::BitStream& stream) {
        T temp{};
        if (!serialization::deserialize(stream, temp))
            return false;
        value_ = std::move(temp);
        dirty_ = true;
        return true;
    }

    /// Field-level delta: write a single bit indicating whether the value
    /// differs from @p baseline, followed by the full value only if changed.
    void serialize_delta(serialization::BitStream& stream, const T& baseline) const {
        bool changed = (value_ != baseline);
        stream.write_bool(changed);
        if (changed) {
            serialize(stream);
        }
    }

    /// Field-level delta: read the changed bit. If changed, update value_
    /// and @p baseline. Returns false on stream underflow.
    bool deserialize_delta(serialization::BitStream& stream, T& baseline) {
        bool changed = false;
        if (!stream.read_bool(changed))
            return false;
        if (changed) {
            if (!deserialize(stream))
                return false;
            baseline = value_;
        }
        return true;
    }

private:
    T value_{};
    bool dirty_ = true;
};

// ── Snapshot history (server-side) ──────────────────────────────────────────

/// Non-owning view of an entity's serialized state within a snapshot blob.
struct EntitySnapshot {
    NetworkId id = 0;
    std::span<const std::uint8_t> data;
};

class SnapshotHistory {
public:
    static constexpr std::size_t MAX_SNAPSHOTS = 128;

    /// Store a snapshot. The @p entities list is expected to reference a
    /// contiguous blob that outlives this call (the history copies the blob).
    void store(std::uint16_t snapshot_id, std::span<const EntitySnapshot> entities);

    /// Retrieve a previously stored snapshot. Returns nullptr if too old or unknown.
    [[nodiscard]] const std::vector<EntitySnapshot>* retrieve(std::uint16_t snapshot_id) const;

private:
    struct Snapshot {
        std::uint16_t id = 0;
        std::vector<std::uint8_t> blob;       ///< Owns the serialized entity data.
        std::vector<EntitySnapshot> entities; ///< Spans into @p blob.
    };
    std::array<Snapshot, MAX_SNAPSHOTS> snapshots_{};
    std::size_t count_ = 0;
    std::size_t head_ = 0; ///< Index of the most recently stored snapshot.
};

// ── Client-side snapshot buffer (Phase 9) ───────────────────────────────────

/// Ring buffer of received snapshots for client-side interpolation.
class ClientSnapshotBuffer {
public:
    static constexpr std::size_t MAX_SNAPSHOTS = 128;

    struct Snapshot {
        std::uint16_t snapshot_id = 0;
        float receive_time = 0.0f;
        std::vector<std::uint8_t> blob;       ///< Owns the serialized entity data.
        std::vector<EntitySnapshot> entities; ///< Spans into @p blob.
    };

    void store(std::uint16_t snapshot_id, float receive_time, std::span<const EntitySnapshot> entities);

    /// Find the two snapshots bracketing @p target_time.
    /// Returns false if fewer than two snapshots are stored.
    /// @p t is the blend factor in [0, 1] (clamped at edges).
    [[nodiscard]] bool find_bracketing(float target_time, const Snapshot*& out_older, const Snapshot*& out_newer,
                                       float& t) const;

    /// Search for an entity inside a specific snapshot. Returns an empty span if absent.
    [[nodiscard]] std::span<const std::uint8_t> find_entity(const Snapshot& snap, NetworkId net_id) const;

    void clear();

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::array<Snapshot, MAX_SNAPSHOTS> snapshots_{};
    std::size_t count_ = 0;
    std::size_t head_ = 0; ///< Index of the most recently stored snapshot.
};

// ── Forward declarations for common NetworkVariable instantiations ──────────

// (definitions are in network_replication.cpp to avoid header bloat)

/// Server-authoritative replication manager.
///
/// Features (Phase 6 MVP):
///   - Dirty-entity tracking: only changed entities are sent
///   - Periodic full sync: every N snapshots all entities are resent
///   - Unreliable channel: state sync tolerates loss, full sync recovers
///   - Variable tick rate: each component type can specify its own interval
///
/// Features (Phase 7 — Delta Compression):
///   - Snapshot history: last 128 snapshots kept on the server
///   - Per-client ack tracking: clients ack received snapshots
///   - Entity-level delta: only entities whose bytes changed since the
///     client's baseline are included in the packet
///   - Field-level delta: NetworkVariable<T> sends a 1-bit "changed" flag
///     followed by the value only when necessary
///
/// Features (Phase 8 — Interest Management):
///   - Per-client interest filter: only entities that pass the filter are
///     replicated to that client
///   - Visible-entity tracking: entities entering interest are force-sent
///     regardless of delta; entities leaving interest are dropped
///
/// Features (Phase 10 — Client Prediction Plumbing):
///   - Prediction mode: received snapshots are not auto-applied; instead a
///     callback is invoked so the game layer can reconcile predicted state
///   - Interpolation delay: client can render behind the server by a fixed
///     time window to smooth out jitter
///
/// Packet format (unreliable, system message type 0x20):
///   [snapshot_id: std::uint16_t]
///   [num_entities: std::uint16_t]
///   for each entity:
///     [net_id: std::uint64_t]
///     [data_len: std::uint16_t]
///     [data: BitStream blob]
class NetworkReplicationManager {
public:
    NetworkReplicationManager() = default;
    ~NetworkReplicationManager() = default;

    NetworkReplicationManager(const NetworkReplicationManager&) = delete;
    NetworkReplicationManager& operator=(const NetworkReplicationManager&) = delete;

    // ── Configuration ────────────────────────────────────────────────────────

    void set_bridge(INetworkReplicationBridge* bridge);
    void set_entity_manager(NetworkEntityManager* entity_mgr);

    /// How many snapshots between forced full-syncs (0 = never).
    void set_full_sync_interval(std::uint16_t interval) noexcept;

    /// Target tick rate for replication (Hz).
    void set_tick_rate(float hz) noexcept;

    /// Maximum age (in snapshots) of a baseline before a full sync is forced.
    void set_max_baseline_age(std::uint16_t age) noexcept;

    // ── Interest management (Phase 8) ────────────────────────────────────────

    /// Generic interest filter. Return true if @p entity should be replicated
    /// to @p client. If no filter is set, all entities are replicated.
    using InterestFilter = std::function<bool(NetworkId entity, ClientId client)>;
    void set_interest_filter(InterestFilter filter);

    // ── Dirty tracking (called by ECS / game code) ───────────────────────────

    void mark_dirty(NetworkId net_id);

    // ── Server: replication tick ─────────────────────────────────────────────

    /// Call at the desired tick rate (e.g. 30 Hz or 60 Hz).
    void server_tick(float delta_time, NetworkManager& net);

    /// Process a snapshot-ack from a client.
    void on_snapshot_ack(ClientId client, std::uint16_t snapshot_id);

    /// Called by NetworkManager when a client connects.
    void on_client_connected(ClientId client);

    /// Called by NetworkManager when a client disconnects.
    void on_client_disconnected(ClientId client);

    // ── Client: apply received state ─────────────────────────────────────────

    /// Process an incoming delta-state message.
    void on_receive_delta(const std::uint8_t* data, std::size_t len);

    /// Call on the client to send acks back to the server.
    void client_tick(float delta_time, NetworkManager& net);

    // ── Prediction (Phase 10) ────────────────────────────────────────────────

    /// When enabled, received snapshots are NOT automatically applied via
    /// the bridge. Instead @p callback is invoked for each entity so the
    /// game layer can compare server state against its prediction and rewind
    /// / replay if necessary.
    using SnapshotReceivedCallback = std::function<void(NetworkId net_id, serialization::BitStream& stream)>;
    void set_prediction_mode(bool enabled);
    void set_snapshot_received_callback(SnapshotReceivedCallback cb);

    /// How far behind the latest snapshot the client should render (seconds).
    /// Default 0.0 (no interpolation delay).
    void set_interpolation_delay(float seconds) noexcept;

    /// The tick of the latest received snapshot (0 if none).
    [[nodiscard]] std::uint16_t latest_received_snapshot() const noexcept;

    // ── Interpolation (Phase 9) ──────────────────────────────────────────────

    /// Enable snapshot buffering and interpolation.
    void set_interpolation_enabled(bool enabled) noexcept;
    [[nodiscard]] bool interpolation_enabled() const noexcept;

    /// Call every render frame to apply interpolated state to the bridge.
    void client_interpolate(float render_time);

    /// Query interpolated state for a single entity without applying it.
    /// The returned BitStreams point into the internal buffer and are only valid
    /// until the next store or until the buffer wraps.
    [[nodiscard]] bool query_interpolated_entity(NetworkId net_id, float render_time, serialization::BitStream& older,
                                                 serialization::BitStream& newer, float& t) const;

    // ── Queries ──────────────────────────────────────────────────────────────

    [[nodiscard]] std::uint16_t current_snapshot_id() const noexcept;
    [[nodiscard]] std::size_t dirty_count() const noexcept;

private:
    INetworkReplicationBridge* bridge_ = nullptr;
    NetworkEntityManager* entity_mgr_ = nullptr;

    std::unordered_set<NetworkId> dirty_entities_;
    std::uint16_t snapshot_id_ = 0;
    std::uint16_t full_sync_interval_ = 10; // every 10th snapshot
    std::uint16_t max_baseline_age_ = 64;   // force full sync if baseline > 64 snaps old
    float tick_rate_ = 30.0f;
    float accumulator_ = 0.0f;

    SnapshotHistory snapshot_history_;

    struct ClientReplicationState {
        std::uint16_t last_acked_snapshot = 0;
        bool has_ack = false;
        std::unordered_set<NetworkId> visible_entities;
    };
    std::unordered_map<ClientId, ClientReplicationState> client_states_;

    InterestFilter interest_filter_;

    // Client-side state
    std::uint16_t last_received_snapshot_ = 0;
    float client_ack_accumulator_ = 0.0f;

    // Interpolation state (Phase 9)
    bool interpolation_enabled_ = false;
    ClientSnapshotBuffer client_snapshot_buffer_;
    float client_time_ = 0.0f;

    // Prediction state (Phase 10)
    bool prediction_mode_ = false;
    SnapshotReceivedCallback snapshot_cb_;
    float interpolation_delay_ = 0.0f;
    std::uint16_t latest_received_snapshot_ = 0;

    // Scratch buffers reused every tick to avoid allocations on hot paths.
    serialization::BitStream scratch_stream_;
    std::vector<std::uint8_t> scratch_blob_;
    std::vector<EntitySnapshot> scratch_snap_;
    std::vector<EntitySnapshot> scratch_delta_;
    std::vector<EntitySnapshot> scratch_filtered_;

    void build_and_send_snapshot(NetworkManager& net, bool full_sync);
    void send_snapshot_to_client(ClientId client, const std::vector<EntitySnapshot>& entities, NetworkManager& net);
};

} // namespace systems::leal::campello_net

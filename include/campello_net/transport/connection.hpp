#pragma once

#include "campello_net/transport/address.hpp"
#include "campello_net/transport/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace systems::leal::campello_net::transport {

/// A single network peer connection with channel-based messaging,
/// reliability, bandwidth limiting, and priority queuing.
class Connection {
public:
    enum class State { Disconnected, Connecting, Connected, Disconnecting };

    Connection() = default;
    explicit Connection(Address address);

    // ── Lifecycle ───────────────────────────────────────────────────────────

    void update(double current_time);

    // ── Incoming ────────────────────────────────────────────────────────────

    void on_packet_received(const PacketHeader& hdr, const uint8_t* payload, std::size_t len);

    // ── Outgoing ────────────────────────────────────────────────────────────

    /// Queue a user message. Higher priority values are sent first (default 128).
    void queue_message(const uint8_t* data, std::size_t len, PacketReliability reliability,
                       uint8_t priority = 128);

    /// Generate the next packet to send. Returns false when nothing to send.
    bool generate_packet(PacketHeader& out_hdr, std::vector<uint8_t>& out_payload);

    // ── Receiving user data ─────────────────────────────────────────────────

    bool pop_message(uint8_t* buffer, std::size_t max_len, std::size_t& out_len);

    // ── State & stats ───────────────────────────────────────────────────────

    [[nodiscard]] State state() const noexcept { return state_; }
    void set_state(State s) { state_ = s; }

    [[nodiscard]] const Address& address() const noexcept { return address_; }
    void set_address(Address addr) { address_ = std::move(addr); }

    [[nodiscard]] float rtt() const noexcept { return smoothed_rtt_; }
    [[nodiscard]] float jitter() const noexcept { return jitter_; }

    [[nodiscard]] double last_recv_time() const noexcept { return last_recv_time_; }
    [[nodiscard]] double last_send_time() const noexcept { return last_send_time_; }

    void mark_received(double t) { last_recv_time_ = t; }
    void mark_sent(double t) { last_send_time_ = t; }

    [[nodiscard]] bool should_send_keepalive(double current_time) const noexcept {
        return state_ == State::Connected && (current_time - last_send_time_ > 0.1);
    }

    // ── Bandwidth & priority ────────────────────────────────────────────────

    void set_channel_bandwidth_limit(PacketReliability reliability, std::uint32_t bytes_per_second);
    void set_global_bandwidth_limit(std::uint32_t bytes_per_second);

    // ── Internal helpers (exposed for transport use) ────────────────────────

    struct Channel {
        std::uint16_t local_seq = 0;
        std::uint16_t remote_seq = 0xFFFF;
        std::uint32_t ack_bits = 0;

        struct PendingPacket {
            std::uint16_t sequence;
            double send_time;
            std::uint8_t retries;
            std::vector<std::uint8_t> data;
            bool fragmented;
        };
        std::vector<PendingPacket> pending;

        struct BufferedPacket {
            std::uint16_t sequence;
            std::uint8_t frag_index;
            std::uint8_t frag_count;
            std::vector<std::uint8_t> data;
        };
        std::vector<BufferedPacket> receive_buffer;

        // Phase 3: priority send queue + bandwidth
        struct QueuedMessage {
            std::uint8_t priority;
            std::vector<std::uint8_t> data;
        };
        std::vector<QueuedMessage> send_queue;
        std::uint32_t bandwidth_limit = 0;      // bytes/sec, 0 = unlimited
        std::uint32_t bytes_sent_this_second = 0;
    };

    [[nodiscard]] Channel& channel(std::size_t idx) { return channels_[idx]; }
    [[nodiscard]] const Channel& channel(std::size_t idx) const { return channels_[idx]; }

private:
    Address address_;
    State state_ = State::Disconnected;
    double last_recv_time_ = 0.0;
    double last_send_time_ = 0.0;
    float smoothed_rtt_ = 0.1f;
    float rtt_variance_ = 0.05f;
    float jitter_ = 0.0f;

    std::array<Channel, 4> channels_{};
    std::vector<std::vector<std::uint8_t>> receive_queue_;
    std::size_t receive_read_idx_ = 0;

    // Bandwidth
    std::uint32_t global_bandwidth_limit_ = 0;
    std::uint32_t bytes_sent_this_second_ = 0;
    double last_bandwidth_reset_ = 0.0;

    // Fragment reassembly
    struct FragmentAssembly {
        Address address;
        std::uint16_t base_sequence = 0;
        std::uint8_t frag_count = 0;
        std::uint8_t received_mask = 0;
        double start_time = 0.0;
        std::vector<std::vector<std::uint8_t>> fragments;
    };
    std::vector<FragmentAssembly> fragment_assemblies_;

    void process_acks(std::uint8_t ch, std::uint16_t ack, std::uint32_t ack_bits);
    void resend_pending(double current_time);
    void process_payload(const PacketHeader& hdr, const uint8_t* payload, std::size_t len);
    bool build_packet_from_message(Channel& ch, PacketReliability reliability, const Channel::QueuedMessage& msg,
                                   PacketHeader& out_hdr, std::vector<std::uint8_t>& out_payload);

    FragmentAssembly& find_or_create_fragment_assembly(std::uint16_t sequence, std::uint8_t frag_count);
    void remove_fragment_assembly(std::uint16_t sequence);
    void cleanup_fragments(double current_time);
};

} // namespace systems::leal::campello_net::transport

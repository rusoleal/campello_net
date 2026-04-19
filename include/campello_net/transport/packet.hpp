#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace systems::leal::campello_net::transport {

/// Reliability semantics for packet delivery.
enum class PacketReliability : uint8_t {
    Unreliable = 0,      ///< Fire-and-forget. May be lost or reordered.
    ReliableOrdered,     ///< Guaranteed delivery, in-order.
    ReliableUnordered,   ///< Guaranteed delivery, any order.
    UnreliableSequenced, ///< Unreliable, but drop stale packets.
};

/// Internal packet types (used on the wire).
enum class PacketType : uint8_t {
    User = 0,
    Handshake,
    Disconnect,
    Ack,
    Fragment,
};

/// Wire header for every packet.
struct PacketHeader {
    static constexpr uint16_t PROTOCOL_ID = 0xCA17;
    static constexpr std::size_t SIZE = 16;

    uint16_t protocol_id = PROTOCOL_ID;
    uint8_t packet_type = 0;  // underlying PacketType
    uint8_t flags = 0;        // reliability << 2 | channel
    uint16_t sequence = 0;    // channel sequence number
    uint16_t ack = 0;         // last received sequence
    uint32_t ack_bits = 0;    // bitmask of prior 32 sequences
    uint16_t payload_len = 0; // bytes following header
    uint8_t frag_index = 0;   // 0 = not fragmented
    uint8_t frag_count = 0;   // total fragments

    [[nodiscard]] PacketReliability reliability() const noexcept;
    [[nodiscard]] uint8_t channel() const noexcept;

    void set_reliability(PacketReliability r) noexcept;
    void set_channel(uint8_t ch) noexcept;

    [[nodiscard]] bool serialize(uint8_t* buffer, std::size_t max_len) const noexcept;
    [[nodiscard]] bool deserialize(const uint8_t* buffer, std::size_t len) noexcept;
};

/// Maximum size of a UDP payload we are willing to send.
inline constexpr std::size_t MAX_PACKET_SIZE = 1200;

/// User payload capacity inside a single non-fragmented packet.
inline constexpr std::size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - PacketHeader::SIZE;

} // namespace systems::leal::campello_net::transport

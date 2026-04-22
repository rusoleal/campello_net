#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace systems::leal::campello_net::transport {

/// @brief Reliability semantics for packet delivery.
enum class PacketReliability : std::uint8_t {
    Unreliable = 0,      ///< Fire-and-forget. May be lost or reordered.
    ReliableOrdered,     ///< Guaranteed delivery, in-order.
    ReliableUnordered,   ///< Guaranteed delivery, any order.
    UnreliableSequenced, ///< Unreliable, but stale packets are dropped.
};

/// @brief Internal packet types (used on the wire).
enum class PacketType : std::uint8_t {
    User = 0,   ///< Application-level payload.
    Handshake,  ///< Connection handshake.
    Disconnect, ///< Graceful disconnect notification.
    Ack,        ///< Acknowledgment of received sequences.
    Fragment,   ///< Fragment of a larger message.
};

/// @brief Wire header for every packet.
///
/// All multi-byte fields are in network (big-endian) byte order.
/// Total header size is 16 bytes.
struct PacketHeader {
    static constexpr std::uint16_t PROTOCOL_ID = 0xCA17; ///< Magic protocol identifier.
    static constexpr std::size_t SIZE = 16;              ///< Header size in bytes.

    std::uint16_t protocol_id = PROTOCOL_ID; ///< Must match PROTOCOL_ID.
    std::uint8_t packet_type = 0;            ///< Underlying PacketType value.
    std::uint8_t flags = 0;                  ///< reliability << 2 | channel
    std::uint16_t sequence = 0;              ///< Channel-local sequence number.
    std::uint16_t ack = 0;                   ///< Last received sequence number.
    std::uint32_t ack_bits = 0;              ///< Bitmask of prior 32 sequences.
    std::uint16_t payload_len = 0;           ///< Bytes following this header.
    std::uint8_t frag_index = 0;             ///< Fragment index (0 = not fragmented).
    std::uint8_t frag_count = 0;             ///< Total number of fragments.

    /// @return The reliability level encoded in flags.
    [[nodiscard]] PacketReliability reliability() const noexcept;
    /// @return The channel index encoded in flags.
    [[nodiscard]] std::uint8_t channel() const noexcept;

    /// @brief Encode reliability into flags.
    void set_reliability(PacketReliability r) noexcept;
    /// @brief Encode channel index into flags.
    void set_channel(std::uint8_t ch) noexcept;

    /// @brief Serialize the header into a byte buffer.
    /// @param buffer Destination buffer.
    /// @param max_len Available bytes in buffer (must be >= SIZE).
    /// @return false if max_len is too small.
    [[nodiscard]] bool serialize(std::uint8_t* buffer, std::size_t max_len) const noexcept;

    /// @brief Deserialize the header from a byte buffer.
    /// @param buffer Source buffer.
    /// @param len Available bytes in buffer (must be >= SIZE).
    /// @return false if len is too small.
    [[nodiscard]] bool deserialize(const std::uint8_t* buffer, std::size_t len) noexcept;
};

/// @brief Maximum size of a UDP payload we are willing to send.
inline constexpr std::size_t MAX_PACKET_SIZE = 1200;

/// @brief User payload capacity inside a single non-fragmented packet.
inline constexpr std::size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - PacketHeader::SIZE;

} // namespace systems::leal::campello_net::transport

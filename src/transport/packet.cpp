#include "campello_net/transport/packet.hpp"

namespace systems::leal::campello_net::transport {

PacketReliability PacketHeader::reliability() const noexcept {
    return static_cast<PacketReliability>((flags >> 2) & 0x3);
}

std::uint8_t PacketHeader::channel() const noexcept {
    return flags & 0x3;
}

void PacketHeader::set_reliability(PacketReliability r) noexcept {
    flags = (flags & 0x3) | ((static_cast<std::uint8_t>(r) & 0x3) << 2);
}

void PacketHeader::set_channel(std::uint8_t ch) noexcept {
    flags = (flags & ~0x3) | (ch & 0x3);
}

bool PacketHeader::serialize(std::uint8_t* buffer, std::size_t max_len) const noexcept {
    if (max_len < SIZE)
        return false;
    buffer[0] = static_cast<std::uint8_t>(protocol_id >> 8);
    buffer[1] = static_cast<std::uint8_t>(protocol_id & 0xFF);
    buffer[2] = packet_type;
    buffer[3] = flags;
    buffer[4] = static_cast<std::uint8_t>(sequence >> 8);
    buffer[5] = static_cast<std::uint8_t>(sequence & 0xFF);
    buffer[6] = static_cast<std::uint8_t>(ack >> 8);
    buffer[7] = static_cast<std::uint8_t>(ack & 0xFF);
    buffer[8] = static_cast<std::uint8_t>(ack_bits >> 24);
    buffer[9] = static_cast<std::uint8_t>(ack_bits >> 16);
    buffer[10] = static_cast<std::uint8_t>(ack_bits >> 8);
    buffer[11] = static_cast<std::uint8_t>(ack_bits & 0xFF);
    buffer[12] = static_cast<std::uint8_t>(payload_len >> 8);
    buffer[13] = static_cast<std::uint8_t>(payload_len & 0xFF);
    buffer[14] = frag_index;
    buffer[15] = frag_count;
    return true;
}

bool PacketHeader::deserialize(const std::uint8_t* buffer, std::size_t len) noexcept {
    if (len < SIZE)
        return false;
    protocol_id = (static_cast<std::uint16_t>(buffer[0]) << 8) | buffer[1];
    packet_type = buffer[2];
    flags = buffer[3];
    sequence = (static_cast<std::uint16_t>(buffer[4]) << 8) | buffer[5];
    ack = (static_cast<std::uint16_t>(buffer[6]) << 8) | buffer[7];
    ack_bits = (static_cast<std::uint32_t>(buffer[8]) << 24) | (static_cast<std::uint32_t>(buffer[9]) << 16) |
               (static_cast<std::uint32_t>(buffer[10]) << 8) | buffer[11];
    payload_len = (static_cast<std::uint16_t>(buffer[12]) << 8) | buffer[13];
    frag_index = buffer[14];
    frag_count = buffer[15];
    return true;
}

} // namespace systems::leal::campello_net::transport

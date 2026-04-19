#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace systems::leal::campello_net::transport {

/// Delivery reliability guarantee for a message.
enum class Delivery : std::uint8_t {
    Unreliable,        ///< May be lost or arrive out of order.
    UnreliableSequenced, ///< Unreliable, but old packets are dropped if newer ones arrived.
    ReliableOrdered,   ///< Guaranteed delivery, in order.
    ReliableUnordered, ///< Guaranteed delivery, may arrive out of order.
};

/// A user-level message envelope.
struct Message {
    Delivery delivery = Delivery::Unreliable;
    std::uint8_t channel = 0; ///< Logical channel (0-3).
    std::vector<std::uint8_t> payload;
};

} // namespace systems::leal::campello_net::transport

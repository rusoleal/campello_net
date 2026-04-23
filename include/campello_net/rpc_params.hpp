#pragma once

#include <cstdint>

namespace systems::leal::campello_net {

using ClientId = std::uint64_t;

/// Authority level required to invoke an RPC.
enum class RpcAuthority {
    Anyone,     ///< Any connected client can invoke (default).
    ServerOnly, ///< Only the server (sender == 0) can invoke; client→server rejected.
};

/// Context passed to every RPC handler.
struct RpcParams {
    /// The peer that sent this RPC (0 when invoked by the server).
    ClientId sender = 0;

    /// Server timestamp when the RPC was processed (seconds since epoch).
    /// On clients this is the local network_time() estimate.
    double server_timestamp = 0.0;

    /// Estimated round-trip time of the sender, in seconds.
    /// Only meaningful on the server; 0 on clients.
    float sender_rtt = 0.0f;
};

} // namespace systems::leal::campello_net

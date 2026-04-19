#pragma once

#include <cstdint>

namespace systems::leal::campello_net {

/// Per-client network statistics.
///
/// All counters are monotonically increasing for the lifetime of the connection.
/// Bandwidth fields are exponential moving averages updated during `poll()`.
struct NetStats {
    // ── Cumulative counters ──
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;

    // ── Bandwidth (bytes/sec, EMA) ──
    float bandwidth_out = 0.0f;
    float bandwidth_in = 0.0f;

    // ── Connection quality ──
    float rtt = 0.0f;
    float packet_loss = 0.0f;
};

} // namespace systems::leal::campello_net

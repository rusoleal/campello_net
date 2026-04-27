# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.0] - 2026-04-27

### Added
- **WebAssembly / Emscripten support**: `EmscriptenWebSocketTransport` wraps the browser's native `WebSocket` API for client-only WASM builds.
- **WASM example**: `examples/wasm_client/` with HTML/JS interop and a Python dev server.
- **CI**: GitHub Actions Emscripten build job (`build-wasm`).

### Changed
- **Transport refactoring**: Extracted `address.cpp` and `packet.cpp` from `udp_transport.cpp` for cleaner platform-specific separation.
- **Build system**: CMake now conditionally compiles `udp_transport.cpp` / `lan_discovery.cpp` on native platforms and `emscripten_websocket_transport.cpp` on Emscripten; LAN discovery is excluded from WASM builds.

## [0.1.0] - 2026-04-23

### Added
- Initial release of `campello_net` — a C++20 multiplatform multiplayer networking library.
- **Transport layer**: UDP transport (WinSock2 / BSD sockets), loopback transport, encrypted transport (ChaCha20-Poly1305), and network simulator with configurable latency, jitter, packet loss, and duplication.
- **Serialization**: `BitStream` with bit-packing, varints, half-floats, quaternion smallest-three compression, and delta encoding.
- **Connection management**: Server / Client / Host modes, connection approval, graceful disconnect, timeout detection, and keep-alive heartbeats.
- **Entity replication**: `NetworkEntityManager` with server-authoritative spawn/despawn, ownership, late-joiner catch-up, and prefab registry.
- **Component replication**: `NetworkReplicationManager` with delta compression, snapshot history (128-entry ring buffer), per-client baselines, and full-sync fallback.
- **Interest management**: `InterestFilter` callbacks, per-client visibility tracking, and `SpatialInterestManager` with grid-based spatial partitioning and configurable relevancy radius.
- **RPCs**: Type-safe `RpcManager` with variadic `invoke_client` / `invoke_server` / `invoke_broadcast`, `RpcParams` (sender, timestamp, RTT), authority checks, and per-RPC rate limiting.
- **Prediction & interpolation**: `InputBuffer` (256-tick ring buffer), lag compensation with rewind-tick history, snapshot interpolation with `ClientSnapshotBuffer`, and extrapolation hooks.
- **Clock sync**: `NetworkTime` with NTP-style exchange, `NetworkClock` discrete tick counter, and tick alignment.
- **Security**: HMAC-SHA256 connection tokens, per-client token-bucket rate limiting (messages/sec, bytes/sec, RPCs/sec), max packet size enforcement, and replay-protected encrypted transport.
- **Discovery**: UDP broadcast LAN beacon discovery (`LanDiscovery`) with live player count updates.
- **Multi-transport**: `set_transport()` / `add_transport()` with cross-transport routing and per-client transport tracking.
- **Stats & logging**: `NetStats` per connection (bandwidth EMA, RTT, packet loss), compile-time log levels, and runtime log callbacks.
- **Examples**: echo, chat, cubes (replicated entities with spatial culling), pong (2-player with interpolation), and stress_test (benchmark tool).
- **Tests**: 155 test cases with 1,889 assertions covering transport, serialization, replication, RPCs, crypto, clock sync, memory auditing, and network simulation.

[0.2.0]: https://github.com/rusoleal/campello_net/releases/tag/v0.2.0
[0.1.0]: https://github.com/rusoleal/campello_net/releases/tag/v0.1.0

# AGENTS.md — campello_net

> Agent-focused instructions for working in the `campello_net` repository.

## Project Context

This repository implements **campello_net**, the multiplayer networking module for the Campello C++20 game engine. It must integrate cleanly with `campello_core` (ECS) but remain usable as a standalone networking library.

**Key constraints:**
- C++20 minimum. Use concepts, `std::span`, designated initializers, and structured bindings where they improve clarity.
- Zero-allocation on hot paths. Pool allocators for packets; no `std::malloc` during `tick()` or `poll()`.
- Multiplatform: Windows (WinSock2), Linux/macOS (BSD sockets), iOS/Android (same BSD), WebAssembly (browser WebSocket).
- Header-first for small utilities; `.cpp` files for platform-specific implementations and heavy logic.

---

## Build System

- **CMake 3.25+** is the only supported build system.
- Targets:
  - `campello_net` — static library
  - `campello_net_tests` — test runner (Catch2)
  - `campello_net_examples` — example binaries (gated by `CAMPELLO_NET_BUILD_EXAMPLES`)
- Compiler flags: `-Wall -Wextra -Wpedantic` on Clang/GCC; `/W4 /permissive-` on MSVC.
- C++ standard is **locked to 20**. Do not use C++23 features.

### Common Commands

```bash
# Configure
cmake -B build -DCAMPELLO_NET_BUILD_TESTS=ON -DCAMPELLO_NET_BUILD_EXAMPLES=ON

# Build
cmake --build build --parallel

# Test
./build/tests/campello_net_tests

# Format (enforced in CI)
clang-format -i src/**/*.cpp include/**/*.hpp
```

---

## Directory Layout

```
campello_net/
├── CMakeLists.txt
├── include/campello_net/
│   ├── network_manager.hpp              # Main entry point
│   ├── network_entity.hpp               # Network ID bridge / spawn-despawn
│   ├── network_replication.hpp          # Replication manager, snapshots, delta compression
│   ├── rpc_manager.hpp                  # RPC dispatcher
│   ├── rpc_params.hpp                   # RPC invocation metadata (sender, timestamp, RTT)
│   ├── network_time.hpp                 # NTP-style clock sync
│   ├── network_clock.hpp                # Discrete tick counter with interpolation
│   ├── net_stats.hpp                    # Per-connection bandwidth/packet stats
│   ├── rate_limiter.hpp                 # Token-bucket rate limiter
│   ├── connection_token.hpp             # HMAC-authenticated connection token
│   ├── network_log.hpp                  # Compile-time log level system
│   ├── version.hpp                      # Version macros
│   ├── transport/
│   │   ├── i_transport.hpp              # Transport abstraction
│   │   ├── udp_transport.hpp            # UDP implementation (Win32 + POSIX in one file)
│   │   ├── emscripten_websocket_transport.hpp # Browser WebSocket transport (WASM only)
│   │   ├── loopback_transport.hpp       # In-memory transport for testing
│   │   ├── encrypted_transport.hpp      # ChaCha20-Poly1305 transport wrapper
│   │   ├── network_simulator.hpp        # Artificial latency/packet loss/duplication
│   │   ├── address.hpp                  # IPv6 (v4-mapped) address wrapper
│   │   └── packet.hpp                   # Packet header, reliability enums, MTU constants
│   ├── serialization/
│   │   ├── bit_stream.hpp               # Bit-packing serializer
│   │   ├── serializable.hpp             # Serializable<T> concept + free functions
│   │   └── quantization.hpp             # Float/vector/quaternion quantization
│   ├── replication/
│   │   └── spatial_interest_manager.hpp # Grid-based spatial culling
│   ├── prediction/
│   │   ├── input_buffer.hpp             # Server-side input ring buffer
│   │   └── lag_compensator.hpp          # Lag compensation / rewind tick history
│   ├── discovery/
│   │   └── lan_discovery.hpp            # UDP broadcast LAN service discovery
│   ├── crypto/
│   │   ├── chachapoly.hpp               # ChaCha20-Poly1305 (RFC 8439)
│   │   └── hmac_sha256.hpp              # HMAC-SHA256 helper
│   └── detail/
│       └── config.hpp                   # Compile-time configuration macros
├── src/
│   ├── transport/
│   │   ├── address.cpp                  # Address parsing / formatting (all platforms)
│   │   ├── packet.cpp                   # PacketHeader serialize / deserialize
│   │   ├── udp_transport.cpp            # Platform socket code
│   │   ├── emscripten_websocket_transport.cpp # Browser WebSocket JS interop
│   │   ├── loopback_transport.cpp       # Loopback hub + transport
│   │   ├── encrypted_transport.cpp      # Encryption wrapper implementation
│   │   └── network_simulator.cpp        # Network simulator implementation
│   ├── discovery/
│   │   └── lan_discovery.cpp            # Beacon broadcast + listen
│   ├── prediction/
│   │   └── lag_compensator.cpp          # Rewind tick + snapshot lookup
│   ├── crypto/
│   │   └── chachapoly.cpp               # Standalone ChaCha20-Poly1305
│   │   └── hmac_sha256.cpp              # HMAC-SHA256 implementation
│   ├── network_manager.cpp              # Connection state machine
│   ├── network_entity.cpp               # Entity spawn/destroy/owner handlers
│   ├── network_replication.cpp          # Snapshot building, delta encoding
│   ├── network_log.cpp                  # Log dispatch
│   ├── network_time.cpp                 # Clock sync sample processing
│   ├── rate_limiter.cpp                 # Token bucket logic
│   ├── rpc_manager.cpp                  # RPC packet build/dispatch
│   └── connection_token.cpp             # Token generate/validate
├── tests/
│   ├── test_*.cpp                       # Unit tests (one file per subsystem)
│   ├── test_crypto.cpp                  # ChaCha20-Poly1305 + HMAC-SHA256
│   ├── test_connection_token.cpp        # Token round-trip / expiry / tamper
│   ├── test_encrypted_transport.cpp     # Encrypted transport replay protection
│   ├── test_memory_audit.cpp            # Allocation tracking on hot paths
│   ├── test_network_time.cpp            # Clock synchronization
│   └── test_version.cpp                 # Version macro sanity
├── examples/
│   ├── echo/
│   ├── chat/
│   ├── cubes/
│   ├── pong/
│   └── stress_test/                     # In-process benchmark
└── third_party/                         # Vendored deps (keep minimal)
```

---

## Coding Style

- **Naming:**
  - Types: `PascalCase` (`NetworkManager`, `BitStream`)
  - Functions/variables: `snake_case` (`send_packet`, `client_id`)
  - Macros/constants: `SCREAMING_SNAKE_CASE` (`CAMPELLO_NET_VERSION`)
  - Private members: trailing underscore (`queue_`)
- **Namespaces:** Everything lives in `systems::leal::campello_net` (sub-namespaces: `::serialization`, `::transport`, `::discovery`, `::crypto`).
- **Headers:** Use `#pragma once`. Include order: project, standard library, third party.
- **No exceptions on hot paths.** Use `std::optional`, `expected` (polyfill if needed), or out-parameters for transport errors.
- **No RTTI or dynamic_cast.** Use type-safe IDs and visitor patterns.

---

## Testing Requirements

Every new subsystem must include tests:

- **Transport:** Loopback send/receive, fragmentation, reliable resend under loss.
- **Serialization:** Roundtrip all primitive types, delta compression accuracy.
- **Replication:** Spawn/despawn, delta application, late-joiner catch-up.
- **RPCs:** Registration, dispatch, target filtering, invalid payload handling, authority checks, rate limiting.
- **Crypto:** RFC test vectors, roundtrip, tamper detection, replay rejection.
- **Integration:** Multi-client server session stability (≥5 minutes).

Use the network simulation API (`LoopbackTransport::set_packet_loss(0.05f)`) in tests to validate reliability.

---

## Integration with Campello Core

- `campello_net` must **not** depend on `campello_core` headers in its public API.
- Bridge types (e.g., `NetworkEntity`) use opaque handles (`uint64_t`) or callback interfaces (`INetworkEntityBridge`, `IReplicationBridge`).
- The user wires ECS world changes into `campello_net` via `NetworkEntityManager::set_bridge()`, `NetworkReplicationManager::set_bridge()`, etc.
- This keeps the network library usable for non-ECS projects.

---

## Platform-Specific Notes

| Platform | File / Define | Notes |
|----------|--------------|-------|
| Windows | `src/transport/udp_transport.cpp` | `WSAStartup`/`WSACleanup` managed in `NetworkManager` lifecycle. `#ifdef _WIN32` blocks in single file. |
| Unix | `src/transport/udp_transport.cpp` | `fcntl` for non-blocking; `SIGPIPE` ignored. Same source file as Windows. |
| iOS | Same as Unix | Watch for background socket suspension. |
| WASM (Browser) | `src/transport/emscripten_websocket_transport.cpp` | Client-only via browser `WebSocket` API. `bind()` returns `false`. All messages are reliable-ordered (TCP). |

Use `#ifdef CAMPELLO_NET_PLATFORM_*` guards. Platform detection happens in CMake.
- `CAMPELLO_NET_PLATFORM_WIN32`
- `CAMPELLO_NET_PLATFORM_LINUX`
- `CAMPELLO_NET_PLATFORM_MACOS`
- `CAMPELLO_NET_PLATFORM_IOS`
- `CAMPELLO_NET_PLATFORM_ANDROID`
- `CAMPELLO_NET_PLATFORM_WASM` (Emscripten; `__EMSCRIPTEN__` detected in `config.hpp`)

---

## Completed Feature Phases

| Phase | Feature | Status |
|---|---|---|
| 0-1 | UDP transport, reliable channels, fragmentation, RTT | ✅ |
| 2 | BitStream serialization (varints, half-floats, quaternions) | ✅ |
| 3 | Connection management, bandwidth limiting, network simulator | ✅ |
| 4 | Server/Client/Host modes, clock sync | ✅ |
| 5 | Entity spawning, ownership, late-joiner catch-up | ✅ |
| 6 | Component replication (state sync) | ✅ |
| 7 | Delta compression (baseline snapshots) | ✅ |
| 8 | Interest management / spatial culling | ✅ |
| 9 | RPC system | ✅ |
| 9b | RpcParams (sender, timestamp, RTT), authority checks, per-RPC rate limits | ✅ |
| 10 | Client prediction plumbing (InputBuffer, snapshot callback) | ✅ |
| 11 | Lag compensation (rewind tick + O(1) history lookup) | ✅ |
| 12 | LAN service discovery (UDP broadcast beacons) | ✅ |
| 13 | Multi-transport support (`set_transport`, `add_transport`, cross-transport routing) | ✅ |
| 13b | Rate limiting per client (messages/sec, bytes/sec, RPCs/sec) | ✅ |
| 13c | Connection token authentication (HMAC-SHA256) | ✅ |
| 13d | ChaCha20-Poly1305 encrypted transport | ✅ |
| 14 | Stress Testing & Benchmarks (`examples/stress_test/`) | ✅ |
| 15 | NetStats per-connection + compile-time logging system | ✅ |

## Future Roadmap

### Phase 16 — Documentation, Cleanup & Release
- Final README / AGENTS.md accuracy pass ✅
- Remove dead code (`connection.hpp`) ✅
- Audit public API documentation ✅
- Memory audit, final cleanup ✅
- Release v0.1.0 ✅

### WebAssembly / Browser Support

Browser-based clients are supported via `EmscriptenWebSocketTransport`, which wraps the browser's native `WebSocket` API using Emscripten's `EM_JS` macros.

**Key constraints:**
- **Client-only**: `bind()` returns `false`; a browser cannot listen for raw socket connections.
- **Reliable-ordered only**: WebSocket runs over TCP, so `PacketReliability::Unreliable` cannot be honored.
- **No LAN discovery**: `LanDiscovery` is excluded from WASM builds because browsers cannot send UDP broadcast beacons.
- **No default transport**: `NetworkManager::start()` will fail on WASM if `set_transport()` was not called with a user-provided transport (e.g., `EmscriptenWebSocketTransport` or `LoopbackTransport`).

**Build example (Emscripten):**
```bash
emcmake cmake -B build-wasm -DCAMPELLO_NET_BUILD_TESTS=ON
emmake cmake --build build-wasm --parallel
```

### Bluetooth / BLE Transport (Placeholder)

A separate transport layer for local multiplayer over Bluetooth Low Energy.

**Why it's out of scope for now:**
- Requires completely separate socket APIs (CoreBluetooth on Apple, Android Bluetooth LE APIs)
- BLE has ~20-byte MTU vs UDP's ~1200-byte MTU — fragmentation logic must be rewritten
- Connection model is GATT client/server, not datagram sockets
- Platform-specific permission handling (Bluetooth pairing dialogs)

**If implemented later:**
- Implement `ITransport` for BLE (the interface now supports it via `send_to()`)
- Custom packet fragmentation for BLE MTU limits
- Service UUID discovery instead of IP addresses
- Out-of-band pairing flow (not handled by the library)

## What NOT to Do

- Do not add Boost, Abseil, or other heavy dependencies. Keep dependencies zero or minimal.
- Do not use `std::function` in the message pump (type erasure allocates). Use function pointers + `void* userdata` or templates.
- Do not couple to a specific rendering or physics frame rate. The network tick is independent.
- Do not break the build on any supported platform. If you can't test Web/iOS locally, rely on CI.

---

## Communication

- GitHub Issues: bug reports, feature requests
- GitHub Discussions: architecture questions, design proposals
- No direct pushes to `main`. All changes via PR with CI green.

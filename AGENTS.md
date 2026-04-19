# AGENTS.md — campello_net

> Agent-focused instructions for working in the `campello_net` repository.

## Project Context

This repository implements **campello_net**, the multiplayer networking module for the Campello C++20 game engine. It must integrate cleanly with `campello_core` (ECS) but remain usable as a standalone networking library.

**Key constraints:**
- C++20 minimum. Use concepts, `std::span`, designated initializers, and structured bindings where they improve clarity.
- Zero-allocation on hot paths. Pool allocators for packets; no `std::malloc` during `tick()` or `poll()`.
- Multiplatform: Windows (WinSock2), Linux/macOS (BSD sockets), iOS/Android (same BSD).
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
├── include/campello/net/
│   ├── network_manager.hpp      # Main entry point
│   ├── transport/
│   │   ├── i_transport.hpp      # Transport abstraction
│   │   └── udp_transport.hpp    # Default UDP implementation
│   ├── serialization/
│   │   └── bit_stream.hpp       # Bit-packing serializer
│   ├── replication/
│   │   ├── net_entity.hpp       # Network ID bridge
│   │   ├── replicator.hpp       # Component replication loop
│   │   └── interest_manager.hpp # Relevancy culling
│   ├── rpc/
│   │   └── rpc_dispatcher.hpp   # Type-safe RPC registry
│   └── clock.hpp                # Network time / tick sync
├── src/
│   ├── transport/
│   │   └── udp_transport.cpp    # Platform socket code
│   └── network_manager.cpp      # Connection state machine
├── tests/
│   └── test_*.cpp               # Unit tests (one file per subsystem)
├── examples/
│   ├── echo/
│   ├── chat/
│   ├── cubes/
│   └── pong/
└── third_party/                 # Vendored deps (keep minimal)
```

---

## Coding Style

- **Naming:**
  - Types: `PascalCase` (`NetworkManager`, `BitStream`)
  - Functions/variables: `snake_case` (`send_packet`, `client_id`)
  - Macros/constants: `SCREAMING_SNAKE_CASE` (`CAMPELLO_NET_VERSION`)
  - Private members: trailing underscore (`queue_`)
- **Namespaces:** Everything lives in `campello::net`.
- **Headers:** Use `#pragma once`. Include order: project, standard library, third party.
- **No exceptions on hot paths.** Use `std::optional`, `expected` (polyfill if needed), or out-parameters for transport errors.
- **No RTTI or dynamic_cast.** Use type-safe IDs and visitor patterns.

---

## Testing Requirements

Every new subsystem must include tests:

- **Transport:** Loopback send/receive, fragmentation, reliable resend under loss.
- **Serialization:** Roundtrip all primitive types, delta compression accuracy.
- **Replication:** Spawn/despawn, delta application, late-joiner catch-up.
- **RPCs:** Registration, dispatch, target filtering, invalid payload handling.
- **Integration:** Multi-client server session stability (≥5 minutes).

Use the network simulation API (`net.set_packet_loss(0.05f)`) in tests to validate reliability.

---

## Integration with Campello Core

- `campello_net` must **not** depend on `campello_core` headers in its public API.
- Bridge types (e.g., `NetEntity`) use opaque handles (`uint64_t`) or callback interfaces.
- The user wires ECS world changes into `campello_net` via `NetworkManager::registerSpawnCallback`, etc.
- This keeps the network library usable for non-ECS projects.

---

## Platform-Specific Notes

| Platform | File / Define | Notes |
|----------|--------------|-------|
| Windows | `src/transport/win32_udp.cpp` | `WSAStartup`/`WSACleanup` managed in `NetworkManager` lifecycle. |
| Unix | `src/transport/posix_udp.cpp` | `fcntl` for non-blocking; `SIGPIPE` ignored. |
| iOS | Same as Unix | Watch for background socket suspension. |

Use `#ifdef CAMPELLO_NET_PLATFORM_*` guards. Platform detection happens in CMake.
- `CAMPELLO_NET_PLATFORM_WIN32`
- `CAMPELLO_NET_PLATFORM_LINUX`
- `CAMPELLO_NET_PLATFORM_MACOS`
- `CAMPELLO_NET_PLATFORM_IOS`
- `CAMPELLO_NET_PLATFORM_ANDROID`

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
| 8 | Interest management / culling | ✅ |
| 9 | RPC system | ✅ |
| 10 | Client prediction plumbing (InputBuffer, snapshot callback) | ✅ |
| 11 | Lag compensation (rewind tick + history lookup) | ✅ |
| 12 | LAN service discovery (UDP broadcast beacons) | ✅ |
| 13 | Multi-transport support (`set_transport`, `add_transport`, cross-transport routing) | ✅ |
| 13b | Rate limiting per client (messages/sec, bytes/sec, RPCs/sec) | ✅ |

## Future Roadmap

### Phase 13 — Bluetooth / BLE Transport (Placeholder)

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

### Phase 14 — Stress Testing & Benchmarks
- Spawn 1k/10k entities, measure replication CPU and memory
- Bandwidth profiling under load
- Packet loss simulation endurance test

### Phase 15 — Documentation & Examples
- API reference / usage guide
- Example: simple replicated game (moving cubes with prediction)
- Memory audit, final cleanup

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

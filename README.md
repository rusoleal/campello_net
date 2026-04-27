# campello_net

[![Version](https://img.shields.io/badge/version-0.2.0-blue.svg)](https://github.com/rusoleal/campello_net/releases)

Advanced multiplayer networking library for the [Campello](https://github.com/rusoleal/campello) game engine ecosystem.

`campello_net` is a C++20, multiplatform networking package designed to bring modern multiplayer capabilities to Campello-powered games. It provides everything from low-level reliable UDP to high-level ECS component replication, client-side prediction, and snapshot interpolation — inspired by the best of Unreal Engine's replication system, Unity's Netcode for GameObjects, and Godot's high-level multiplayer API.

---

## ✨ Features

- **🚀 C++20 Modern API**
  Concepts, `std::span`, designated initializers, and zero-overhead abstractions. Zero dynamic allocation on hot paths.

- **🌍 Multiplatform**
  Windows (WinSock2), Linux/macOS/iOS/Android (BSD sockets). Web transport planned.

- **🔌 Transport Abstraction**
  Pluggable transport layer with built-in UDP, loopback (for testing), encrypted (ChaCha20-Poly1305), and network-simulator transports.

- **🧩 ECS-Native Replication**
  Component-level state synchronization via `NetworkEntityManager` + `NetworkReplicationManager`. Works standalone or with `campello_core`.

- **⚡ High Performance**
  Bit-packing serialization, delta compression, spatial interest management, and snapshot interpolation.

- **🎯 Type-Safe RPCs**
  Remote Procedure Calls with `RpcParams` context (sender, timestamp, RTT), authority checks, and per-RPC rate limiting.

- **🎮 Client-Side Prediction**
  Input buffering on the server, lag compensation with rewind-tick history lookup.

- **📊 Snapshot Interpolation**
  Smooth remote entity playback with jitter buffering and extrapolation.

- **🔒 Security Built-In**
  HMAC-SHA256 connection tokens, optional packet encryption, per-client rate limiting, and replay-protection.

- **🛠️ Debugging**
  Network stats per connection, compile-time log levels, and LAN service discovery.

---

## 🏗️ Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Application / Game                       │
├─────────────────────────────────────────────────────────────┤
│  NetworkManager  │  RpcManager  │  SpatialInterestManager  │
├─────────────────────────────────────────────────────────────┤
│  NetworkEntityManager  │  NetworkReplicationManager         │
├─────────────────────────────────────────────────────────────┤
│  SnapshotHistory  │  ClientSnapshotBuffer  │  LagCompensator │
├─────────────────────────────────────────────────────────────┤
│  BitStream  │  InputBuffer  │  NetworkTime / NetworkClock   │
├─────────────────────────────────────────────────────────────┤
│  EncryptedTransport  │  UdpTransport  │  LoopbackTransport  │
└─────────────────────────────────────────────────────────────┘
```

### Design Philosophy

1. **Server Authority First** — The server is the single source of truth. Clients predict; servers correct.
2. **ECS-Centric** — Replication happens at the component level via opaque entity handles. No hard dependency on `campello_core`.
3. **Opt-In Complexity** — Use raw messages for simple games; enable full prediction and snapshots for competitive titles.
4. **Transport Agnostic** — Swap UDP for loopback or an encrypted wrapper without changing game code.

---

## 📦 Installation

> **Note:** `campello_net` is a header-first / static library. Its public API does not depend on `campello_core`, but it provides callback-based bridges for ECS integration.

### CMake

```cmake
include(FetchContent)
FetchContent_Declare(
  campello_net
  GIT_REPOSITORY https://github.com/rusoleal/campello_net.git
  GIT_TAG        v0.2.0   # Pin to a release; use 'main' for latest
)
FetchContent_MakeAvailable(campello_net)

target_link_libraries(your_game PRIVATE campello_net)
```

### Requirements

- C++20 compatible compiler (GCC 12+, Clang 15+, MSVC 2022+)
- CMake 3.25+
- (Optional) `campello_core` for deep ECS replication integration

---

## 🚀 Quick Start

### 1. Start a Server

```cpp
#include <campello_net/network_manager.hpp>
#include <campello_net/rpc_manager.hpp>
#include <iostream>

using namespace systems::leal::campello_net;

int main() {
    NetworkManager net;
    net.start(NetworkManager::Config{
        .mode = NetworkManager::Mode::Server,
        .bind_address = transport::Address("::", 7777),
        .max_clients = 32,
    });

    net.on_client_connected([](ClientId id) {
        std::cout << "Client " << id << " joined\n";
    });

    net.on_client_disconnected([](ClientId id) {
        std::cout << "Client " << id << " left\n";
    });

    RpcManager rpc;
    rpc.set_network_manager(&net);
    net.set_rpc_manager(&rpc);

    while (net.is_active()) {
        net.poll();
    }
    return 0;
}
```

### 2. Connect a Client

```cpp
#include <campello_net/network_manager.hpp>
#include <campello_net/rpc_manager.hpp>
#include <iostream>

using namespace systems::leal::campello_net;

int main() {
    NetworkManager net;
    net.start(NetworkManager::Config{
        .mode = NetworkManager::Mode::Client,
        .server_address = transport::Address("127.0.0.1", 7777),
    });

    RpcManager rpc;
    rpc.set_network_manager(&net);
    net.set_rpc_manager(&rpc);

    while (net.is_active()) {
        net.poll();

        // Pop user messages
        NetworkManager::ReceivedMessage msg;
        while (net.pop_message(msg)) {
            // Process msg.payload from msg.client
        }
    }
    return 0;
}
```

### 3. Register an RPC Handler

```cpp
// Server-side: handle client input
rpc.register_handler(1, [](const RpcParams& params, BitStream& args) {
    float dx = 0.0f, dy = 0.0f;
    deserialize(args, dx);
    deserialize(args, dy);
    apply_input(params.sender, dx, dy);  // params.sender == ClientId
}, RpcAuthority::Anyone);  // or RpcAuthority::ServerOnly

// Client-side: invoke the RPC
rpc.invoke_server(1, 1.0f, 0.0f);
```

### 4. Entity Spawning & Replication

```cpp
// Set up the bridge between net and your game world
NetworkEntityManager entity_mgr;
entity_mgr.set_bridge(&my_game_bridge);
net.set_entity_manager(&entity_mgr);

NetworkReplicationManager repl;
repl.set_bridge(&my_game_bridge);
repl.set_entity_manager(&entity_mgr);
net.set_replication_manager(&repl);

// Server spawns an entity — automatically synced to clients
NetworkId net_id = entity_mgr.spawn(/*prefab*/ 1, /*init_data*/ {});
```

---

## 🧪 Examples

| Example | Description |
|---------|-------------|
| `examples/echo` | Minimal client/server message echo |
| `examples/chat` | Text chat with reliable RPC messaging |
| `examples/cubes` | Replicated moving cubes with spatial culling |
| `examples/pong` | 2-player game with replication and interpolation |

Build examples:

```bash
cmake -B build -DCAMPELLO_NET_BUILD_EXAMPLES=ON
cmake --build build
./build/examples/echo/echo_example
```

---

## 📖 Documentation

- Browse the headers in `include/campello_net/` — every public class and method is documented inline.
- See [`AGENTS.md`](AGENTS.md) for agent-focused build instructions, coding style, and architecture notes.
- See [`todo.md`](todo.md) for the implementation roadmap.

---

## 🗺️ Roadmap

See [`CHANGELOG.md`](CHANGELOG.md) for release notes and [`todo.md`](todo.md) for the implementation roadmap.

High-level milestones:

- **M0 Alpha Core** ✅ — Connections, messaging, server/client/host
- **M1 Beta Sync** ✅ — ECS replication, RPCs, interest management, delta compression
- **M2 Beta Gameplay** ✅ — Prediction, interpolation, clock sync, security (tokens + encryption)
- **M3 Release** ✅ — Stress testing, benchmarks, final documentation, v0.2.0 tagged

---

## 🤝 Ecosystem

`campello_net` is part of the Campello engine family:

| Package | Description |
|---------|-------------|
| [`campello_core`](https://github.com/rusoleal/campello) | Core ECS backbone |
| [`campello_renderer`](https://github.com/rusoleal/campello_renderer) | 3D rendering pipeline |
| [`campello_gpu`](https://github.com/rusoleal/campello_gpu) | Graphics abstraction (WebGPU-style) |
| [`campello_audio`](https://github.com/rusoleal/campello_audio) | Audio playback & processing |
| **`campello_net`** | **Multiplayer networking (this repo)** |

---

## 📄 License

MIT License — see [LICENSE](LICENSE).

---

## 🙋 Contributing

Contributions welcome! Please ensure your PRs include tests and pass the CI build.

For questions, open a [GitHub Discussion](https://github.com/rusoleal/campello/discussions) or file an issue.

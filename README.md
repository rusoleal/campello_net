# campello_net

Advanced multiplayer networking library for the [Campello](https://github.com/rusoleal/campello) game engine ecosystem.

`campello_net` is a C++20, multiplatform networking package designed to bring modern multiplayer capabilities to Campello-powered games. It provides everything from low-level reliable UDP to high-level ECS component replication, client-side prediction, and snapshot interpolation — inspired by the best of Unreal Engine's replication system, Unity's Netcode for GameObjects, and Godot's high-level multiplayer API.

---

## ✨ Features

- **🚀 C++20 Modern API**
  Concepts, coroutines-ready architecture, and zero-overhead abstractions.

- **🌍 Multiplatform**
  Windows, Linux, macOS, iOS, and Android.

- **🔌 Transport Abstraction**
  Pluggable transport layer with built-in reliable UDP and loopback implementations.

- **🧩 ECS-Native Replication**
  Component-level state synchronization designed for `campello_core`'s ECS architecture.

- **⚡ High Performance**
  Bit-packing serialization, delta compression, interest management, and spatial culling.

- **🎯 Type-Safe RPCs**
  Compile-time checked Remote Procedure Calls with multiple targeting modes.

- **🎮 Client-Side Prediction**
  Responsive input with server reconciliation for action-oriented games.

- **📊 Snapshot Interpolation**
  Smooth remote entity playback with jitter buffering and extrapolation.

- **🔒 Security Built-In**
  Connection authentication, rate limiting, input validation, and optional encryption.

- **🛠️ Debugging & Profiling**
  Network stats, packet capture, and profiler hooks for the Campello editor.

---

## 🏗️ Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Application / Game                       │
├─────────────────────────────────────────────────────────────┤
│  NetworkManager  │  RPC Dispatcher  │  Interest Manager    │
├─────────────────────────────────────────────────────────────┤
│  Entity Replication  │  Snapshot System  │  Network Clock    │
├─────────────────────────────────────────────────────────────┤
│  Connection Manager  │  Message Channels  │  BitStream       │
├─────────────────────────────────────────────────────────────┤
│              Transport Layer (UDP / WebSocket)              │
└─────────────────────────────────────────────────────────────┘
```

### Design Philosophy

1. **Server Authority First** — The server is the single source of truth. Clients predict; servers correct.
2. **ECS-Centric** — Replication happens at the component level, fitting Campello's data-oriented design.
3. **Opt-In Complexity** — Use raw messages for simple games; enable full prediction and snapshots for competitive titles.
4. **Transport Agnostic** — Swap UDP for WebSocket or a console SDK without changing game code.

---

## 📦 Installation

> **Note:** `campello_net` is a header-first / static library. It depends on `campello_core` for ECS integration, but its transport and serialization layers are usable standalone.

### CMake

```cmake
include(FetchContent)
FetchContent_Declare(
  campello_net
  GIT_REPOSITORY https://github.com/rusoleal/campello_net.git
  GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(campello_net)

target_link_libraries(your_game PRIVATE campello::net)
```

### Requirements

- C++20 compatible compiler (GCC 12+, Clang 15+, MSVC 2022+)
- CMake 3.25+
- (Optional) `campello_core` for ECS replication features

---

## 🚀 Quick Start

### 1. Start a Server

```cpp
#include <campello/net/network_manager.hpp>

using namespace campello::net;

int main() {
    NetworkManager net;
    net.startServer(7777, /* maxClients */ 32);

    net.onClientConnected = [](ClientId id) {
        std::println("Client {} joined", id);
    };

    net.onClientDisconnected = [](ClientId id) {
        std::println("Client {} left", id);
    };

    while (net.isListening()) {
        net.poll();
        net.tick(); // fixed network tick
    }
    return 0;
}
```

### 2. Connect a Client

```cpp
#include <campello/net/network_manager.hpp>

int main() {
    NetworkManager net;
    net.startClient("127.0.0.1", 7777);

    net.onConnected = [] {
        std::println("Connected to server!");
    };

    while (net.isConnected()) {
        net.poll();
        net.tick();
    }
    return 0;
}
```

### 3. Replicate an ECS Component

```cpp
#include <campello/net/replication.hpp>

// Mark a component as replicated
struct Transform : ReplicatedComponent<Transform> {
    Vec3 position;
    Quat rotation;

    void serialize(BitStream& stream) {
        stream.writeQuantized(position, /* min */ -1000.0f, /* max */ 1000.0f, /* bits */ 20);
        stream.writeSmallestThree(rotation, /* bits */ 16);
    }
};

// On the server: spawn and replicate
Entity player = world.createEntity();
world.add<Transform>(player, Vec3{0, 0, 0});
net.spawn(player); // automatically synced to relevant clients
```

### 4. Call an RPC

```cpp
// Define a server RPC
campello_net_rpc(FireWeapon, Server, (int weaponId, Vec3 direction)) {
    // Runs on server when any client calls it
    if (!validateOwner(sender)) return;
    applyDamage(weaponId, direction);
}

// From client
net.rpc().call<FireWeapon>(weaponId, aimDirection);
```

---

## 🧪 Examples

| Example | Description |
|---------|-------------|
| `examples/echo` | Minimal client/server message echo |
| `examples/chat` | Text chat with reliable messaging |
| `examples/cubes` | 1000 replicated moving cubes |
| `examples/pong` | 2-player game with client prediction |
| `examples/shooter` | Authoritative server with lag compensation |

Build examples:

```bash
cmake -B build -DCAMPELLO_NET_BUILD_EXAMPLES=ON
cmake --build build
./build/examples/echo/echo_example
```

---

## 📖 Documentation

- [API Reference](https://rusoleal.github.io/campello_net) (Doxygen)
- [Architecture Decisions](docs/adr/)
- [Multiplayer Concepts](docs/concepts.md) — prediction, reconciliation, snapshots
- [Integration Guide](docs/integration.md) — wiring into `campello_core`

---

## 🗺️ Roadmap

See [`todo.md`](todo.md) for the full implementation roadmap.

High-level milestones:

- **M0 Alpha Core** — Connections, messaging, server/client/host
- **M1 Beta Sync** — ECS replication, RPCs, interest management
- **M2 Beta Gameplay** — Prediction, interpolation, clock sync, security
- **M3 Release** — Web transport, tooling, production hardening

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

Contributions welcome! Please read the [Campello contribution guidelines](https://github.com/rusoleal/campello/blob/main/CONTRIBUTING.md) and ensure your PRs include tests.

For questions, open a [GitHub Discussion](https://github.com/rusoleal/campello/discussions) or file an issue.

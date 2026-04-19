# Campello Net — Implementation Roadmap

> C++20 multiplatform multiplayer networking library for the Campello game engine ecosystem.

This roadmap is organized in progressive phases. Each phase builds upon the previous one and produces a usable, testable milestone. The architecture draws inspiration from Unreal's actor replication & RPC system, Unity NGO's NetworkVariables + transport abstraction, and Godot's high-level multiplayer API.

---

## Phase 0 — Foundation & Tooling
**Goal:** Repository structure, build system, testing, and CI ready.

- [x] Define `CMakeLists.txt` with C++20 standard, strict warnings, and multiplatform targets (Win/Linux/macOS/iOS/Android)
- [x] Set up folder structure: `include/campello_net/`, `src/`, `tests/`, `examples/`, `third_party/`
- [x] Integrate testing framework (Catch2 v3.7.0 auto-fetched via CMake) and add CI workflow (GitHub Actions)
- [x] Add `.gitignore`, license headers, and clang-format configuration aligned with Campello style
- [x] Create stub `namespace systems::leal::campello_net` namespace and version macros
- [x] Write a minimal echo example to validate the build pipeline end-to-end

**Deliverable:** `campello_net` compiles on all desktop targets; CI green.
**Status:** Complete. 72 tests, 1395 assertions, zero flakes across 10 consecutive runs.

---

## Phase 1 — Transport Abstraction
**Goal:** Pluggable network transport with a UDP-based default implementation.

- [x] Design `ITransport` interface: bind, connect, disconnect, send, receive, poll events
- [x] Implement `UdpTransport` using raw Berkeley sockets / WinSock with IPv4 and IPv6 support
- [x] Add connection-oriented semantics on top of UDP: handshake, keep-alive, disconnect detection
- [x] Implement packet types: unreliable, reliable ordered, reliable unordered, sequenced
- [x] Add MTU awareness (≈1200 bytes conservative), automatic packet fragmentation & reassembly
- [x] Implement congestion control: RTT measurement, sliding window, ACK piggybacking
- [x] Write transport unit tests (loopback, packet loss simulation, fragmentation)

**Deliverable:** Two processes can connect and exchange reliable/unreliable packets over localhost.
**Status:** Complete.

---

## Phase 2 — Bitstream Serialization
**Goal:** Fast, zero-allocation binary serialization for game state and RPC arguments.

- [x] Implement `BitStream` writer/reader with bit-level packing (quantization, compression)
- [x] Add primitive serializers: bool, integers (varint), floats (half/fixed/smallest-three for quats), vectors
- [x] Support `std::string`, `std::vector`, and fixed-size arrays
- [x] Add delta encoding utilities (baseline + changed mask for structs)
- [x] Implement serialize/deserialize concept (`Serializable<T>`) for user-defined types
- [x] Provide endianness normalization (always network byte order on wire)
- [x] Benchmark against naive memcpy to validate bit-packing wins

**Deliverable:** Serialize/deserialize complex structs with <1% overhead vs theoretical minimum.
**Status:** Complete. `BitStream` fully functional with read/write/serialize/deserialize.

---

## Phase 3 — Connection & Message Channels
**Goal:** High-level connection management with prioritized message channels.

- [x] Design `Connection` class wrapping `ITransport`; expose `ClientId` (uint64)
- [x] Implement message channels: 0=ReliableOrdered, 1=ReliableUnordered, 2=Unreliable, 3=UnreliableSequenced
- [x] Per-channel bandwidth limits and priority queue (starvation prevention)
- [x] Connection state machine: Disconnected → Connecting → Connected → Disconnecting
- [x] Event-driven API: `OnClientConnected`, `OnClientDisconnected`, `OnDataReceived`
- [x] Support both client-server and peer-to-peer topologies (abstracted via `NetworkPeer`)
- [x] Heartbeat/ping with moving-average RTT and jitter calculation
- [x] Add network simulation layer (packet loss, latency, jitter, duplication) for testing

**Deliverable:** Multi-client server echo test with simulated 200ms latency and 5% packet loss.
**Status:** Complete.

---

## Phase 4 — Network Manager & Topologies
**Goal:** Unified `NetworkManager` supporting Server, Client, and Host modes.

- [x] Implement `NetworkManager` entry point (instantiable per world)
- [x] Server mode: listen, accept connections, assign `ClientId`, max player slots
- [x] Client mode: connect to server by IP/port, handle connection approval/rejection
- [x] Host mode: local client + server in same process (loopback optimization via `host_local_recv_queue`)
- [x] Connection approval callback (authentication token, version check, ban list)
- [x] Graceful disconnection (flush reliable queue before close) and timeout detection
- [x] Callback-driven architecture to avoid coupling with ECS (`INetworkEntityBridge`, `INetworkReplicationBridge`)
- [x] Add `NetworkTime`: local + remote timeline with clock synchronization (simple NTP-style exchange)

**Deliverable:** A 3-client + 1-server LAN session stays stable for 10 minutes.
**Status:** Complete.

---

## Phase 5 — Network Identity & Entity Spawning
**Goal:** Network-aware entity lifecycle integrated with Campello ECS.

- [x] Define `NetworkId` (uint64) allocation strategy: server-authoritative, sparse range
- [x] Design `NetEntity` bridge between ECS entity and network identity (non-owning handle)
- [x] Server-side network spawn: assign `NetworkId`, broadcast spawn message to relevant clients
- [x] Client-side network spawn: receive spawn message, create local ECS entity from prefab/archetype
- [x] Network destroy: server commands destruction, clients clean up local proxy
- [x] Network prefab/archetype registry: map `PrefabId` → ECS archetype + initial component set
- [x] Handle late-joiner catch-up: spawn all existing net entities in priority order
- [x] Ownership model: server owns by default; ability to assign client ownership per entity

**Deliverable:** Server spawns 100 networked entities; late-joining client receives all within 1 second.
**Status:** Complete.

---

## Phase 6 — Component Replication & Delta Compression
**Goal:** Automatic, delta-compressed state synchronization of ECS components.

- [x] `NetworkVariable<T>`: dirty-tracking replicated scalar with `serialize_delta` / `deserialize_delta`
- [x] Dirty-component tracking: mark components dirty on mutation (`mark_dirty(NetworkId)`)
- [x] Server replication loop (fixed tick rate, 30 Hz): collect dirty components, build delta packets per client
- [x] SnapshotHistory: 128-entry ring buffer of `EntitySnapshot` vectors for baseline tracking
- [x] Per-client delta compression: entity-level byte diff + field-level `NetworkVariable` 1-bit changed flag
- [x] Automatic full-sync fallback when baseline is too old or client has no ack
- [x] Client application of deltas: update local ECS component state
- [x] Delta compression for transforms: position quantization, velocity-acceleration extrapolation hint
- [x] Snapshot acknowledgment: client acks received snapshot IDs for server baseline (`SnapshotAck = 0x21`)
- [x] `NetworkReplicationManager` with `set_bridge`, `set_entity_manager`, interest filter hook

**Deliverable:** 1000 moving entities replicated at 30Hz using <500 KB/s total server upstream.
**Status:** Complete. 52 tests covering `NetworkVariable`, snapshot building, delta application.

---

## Phase 7 — Interest Management (Relevancy & Culling)
**Goal:** Server only sends data clients actually need.

- [x] `InterestFilter` callback: `std::function<bool(NetworkId, ClientId)>` for culling
- [x] Per-client `visible_entities` tracking in `NetworkReplicationManager`
- [x] Newly-visible entity detection: force-send full state on first visibility
- [x] Leave detection: remove entities from interest set when filter returns false
- [x] Network dormancy: entities outside interest set skip replication
- [x] Cull destroyed/despawned entities from interest sets immediately
- [ ] Grid-based spatial partitioning interest manager (default)
- [ ] Configurable relevancy radius per entity type + global max entity cap per client
- [ ] Network priority: sort replication by priority (distance, importance) when bandwidth constrained
- [ ] Observer pattern: client subscribes/unsubscribes to entity updates dynamically

**Deliverable:** 10,000 static entities in world; client only receives updates for ~100 within radius.
**Status:** Core interest filtering complete. Spatial partitioning and priority sorting deferred.

---

## Phase 8 — Remote Procedure Calls (RPCs)
**Goal:** Type-safe, declarative remote function invocation.

- [x] `RpcManager`: registers handlers by `uint16_t` rpc_id
- [x] Type-safe variadic `invoke_client(client, rpc_id, args...)` and `invoke_server(rpc_id, args...)`
- [x] Variadic template argument serialization using `serialization::serialize` / `deserialize`
- [x] Wire format: `[0xCA][0xFE][0x22][rpc_id:2][payload...]`
- [x] Handler signature: `void(ClientId sender, BitStream& args)`
- [x] Reliable delivery via existing transport channels
- [x] Server → Client RPC fan-out with interest filtering integration
- [ ] RPC targets: `Broadcast`, `Owner`, `NotOwner`, `Target(ClientId)` (server routing only)
- [ ] Client → Server RPC validation: ensure sender has authority, rate limiting per RPC type
- [ ] `RpcParams` struct with sender ClientId, timestamp, and delivery metadata

**Deliverable:** Fire-and-forget RPC roundtrip in <1.5× RTT; 10k RPCs/sec without stalls.
**Status:** Core RPC system complete. Advanced routing targets and rate limiting deferred.

---

## Phase 9 — Snapshot System & Interpolation
**Goal:** Smooth rendering of remote state despite network jitter.

- [x] Server snapshot buffer: `SnapshotHistory` stores world state at fixed tick intervals
- [x] Client snapshot receive buffer: replication manager tracks latest received snapshot
- [x] Snapshot delta compression: send only changed entities/components against last acked snapshot
- [x] Snapshot acknowledgment: client acks received snapshot IDs for server baseline
- [x] Interpolation delay: `set_interpolation_delay(seconds)` for render-time smoothing
- [ ] Entity interpolation: interpolate transform between snapshot N-2 and N-1 (render delay = 2× RTT)
- [ ] Extrapolation for missing data: brief prediction using last-known velocity
- [ ] Client-side snapshot buffer keeping last N snapshots (e.g., 2 seconds)

**Deliverable:** 200ms simulated jitter; remote entities move smoothly without visible stuttering.
**Status:** Server snapshot history and delta compression complete. Client-side interpolation/extrapolation deferred to gameplay layer integration.

---

## Phase 10 — Client-Side Prediction & Server Reconciliation
**Goal:** Responsive player input for action games.

- [x] `InputBuffer`: 256-tick ring buffer per client storing inputs indexed by simulation tick
- [x] Wrap-safe tick comparison via `int16_t` arithmetic
- [x] Prediction mode: `set_prediction_mode(true)` routes snapshots to callback instead of auto-applying
- [x] `set_snapshot_received_callback(SnapshotReceivedCallback cb)` for game-layer reconciliation
- [x] Server-side input retrieval: `retrieve(client, tick, out_data)` for authoritative simulation
- [ ] Client prediction: apply local input immediately, simulate ahead of server state
- [ ] Input queue: buffer inputs with tick numbers, send to server
- [ ] Server authoritative correction: server simulates inputs, returns corrected state
- [ ] Reconciliation: on correction mismatch, rewind local state to server snapshot, replay inputs
- [ ] Prediction error smoothing: blend corrected position over a few frames to avoid pops
- [ ] Separate predicted (local) and interpolated (remote) entity representations

**Deliverable:** Player moves instantly on keypress; 150ms latency correction is imperceptible.
**Status:** Server-side input buffering and prediction hooks complete. Full client-side prediction/reconciliation loop deferred to gameplay layer integration.

---

## Phase 11 — Clock Synchronization & Fixed Network Tick
**Goal:** Deterministic timing across all peers.

- [x] `NetworkTime`: local + remote timeline with clock synchronization
- [x] Snapshot ID tied to server tick for unambiguous baselines
- [x] Fixed network tick loop in `server_tick()` / `client_tick()` independent of render framerate
- [ ] `NetworkClock` with statistical RTT filtering (exponential moving average + stddev)
- [ ] Tick alignment: all clients run same tick number (±1) for lockstep-ready modes
- [ ] API: `NetTick current = NetworkTime::tick(); float interp = NetworkTime::interpolationFactor();`

**Deliverable:** 4 clients report synchronized tick within ±2 ticks under normal conditions.
**Status:** Basic tick and snapshot ID sync complete. Statistical clock filtering and lockstep alignment deferred.

---

## Phase 12 — LAN Discovery
**Goal:** Zero-configuration LAN server discovery for local multiplayer.

- [x] `LanDiscovery` dual-mode class (advertiser or listener)
- [x] UDP broadcast beacons on separate `AF_INET` socket
- [x] Beacon format: `[magic: 'CAMP'][version: 1][game_port][max_players][current_players][name_len][name]`
- [x] `start_advertising()` / `start_listening()` with `poll()` for real-time updates
- [x] `set_current_players(count)` updates live player count in broadcast beacons
- [x] `on_beacon_received(callback)` delivers parsed beacon with IPv4-mapped IPv6 address
- [x] Graceful `stop_advertising()` / `stop_listening()` with socket cleanup

**Deliverable:** Client discovers LAN server within 2 seconds of server start.
**Status:** Complete. 77 tests total.

---

## Phase 13 — Security & Rate Limiting
**Goal:** Basic protections against common network exploits.

- [x] `RateLimiter`: token-bucket per client for messages/sec, bytes/sec, RPCs/sec
- [x] `NetworkManager::Config` rate limit fields: `max_messages_per_sec`, `max_bytes_per_sec`, `max_rpcs_per_sec`, `rate_limit_burst`
- [x] Per-client rate limiter auto-configured on connection, checked on every inbound packet
- [x] Max packet size enforcement: oversized packets disconnect the offender
- [x] Max clients enforcement: `ConnectRequest` rejected when `client_count() >= max_clients`
- [x] System messages (handshake, time sync) are never rate-limited; only user data and RPCs
- [x] Rate limiter stats: `messages_allowed/dropped`, `bytes_allowed/dropped`, `rpcs_allowed/dropped`
- [ ] Connection token / HMAC authentication at handshake
- [ ] Input validation: clamp replicated floats/vectors to sane ranges on server
- [ ] Configurable max entity count, max RPC payload size
- [ ] Optional DTLS or ChaCha20-Poly1305 encryption layer over transport
- [ ] Anti-cheat hook API: server-side callbacks to validate movement speed, hit registration, etc.
- [ ] Logging of suspicious events

**Deliverable:** Server stays stable under 10× normal packet rate; excess traffic silently dropped.
**Status:** Core rate limiting complete. Encryption and anti-cheat hooks deferred.

---

## Phase 14 — Multi-Transport & Additional Transports
**Goal:** Support multiple simultaneous transports and platforms where raw UDP is unavailable.

- [x] `ITransport` abstraction with `send_to()`, `get_connection_rtt()`, `get_connection_packet_loss()`
- [x] `NetworkManager` multi-transport support: `set_transport()` + `add_transport()`
- [x] Per-client transport tracking: `ClientEntry` stores `ITransport*` pointer
- [x] Cross-transport broadcast: server sends to all clients regardless of which transport they connected on
- [x] Cross-transport targeted send: `send(ClientId)` routes to the correct transport automatically
- [x] `NetworkSimulator` updated with `send_to()` and per-connection metric forwarding
- [x] `LoopbackTransport` pure in-memory transport for testing and local multiplayer
- [x] `LoopbackHub` shared message router; transports on same hub communicate instantly
- [x] Configurable artificial latency and packet loss on loopback (for testing timing code)
- [ ] Transport-adapter for third-party relays (e.g. custom TURN or proprietary backends)
- [ ] Android/iOS cellular optimization (batching, sensitivity to radio state changes)
- [ ] Bluetooth/BLE transport for ultra-local multiplayer (platform-specific APIs: CoreBluetooth / Android BLE)

**Deliverable:** Server accepts clients on UDP port A and UDP port B simultaneously; messages route correctly.
**Status:** Multi-transport core complete (5 new tests, 77 total). BLE/relays deferred.

---

## Phase 15 — Debugging, Profiling & Tooling
**Goal:** Developers can understand and optimize network behavior.

- [ ] `NetStats` per-connection: bytes in/out, packets lost, RTT, bandwidth estimate
- [ ] Network logging categories: Verbose, Info, Warning, Error (compile-time strip)
- [ ] Packet capture to pcap-compatible or custom format for offline analysis
- [ ] Built-in network profiler API (integrates with campello editor via callbacks)
- [ ] Network visualization hooks: draw relevancy radius, predicted vs interpolated positions
- [ ] Stress-test example: variable entity counts, variable packet loss, benchmark tool

**Deliverable:** Real-time bandwidth graph in example app; clear perf metrics for all phases.
**Status:** Not started.

---

## Phase 16 — Polish, Documentation & Release
**Goal:** Production-ready API with full documentation.

- [ ] Final API review: consistency with Campello naming conventions, header-only where appropriate
- [ ] Write comprehensive Doxygen / API docs
- [ ] Create minimal examples: chat, replicated cubes, authoritative shooter, Pong
- [ ] Integration test with `campello_core` ECS and `campello_renderer`
- [ ] Memory audit: zero leaks, no allocations on hot paths, pool allocators for packets
- [ ] Release v0.1.0 with semantic versioning

**Deliverable:** Tagged release; external developer can add multiplayer to a Campello game in <1 hour.
**Status:** Not started.

---

## Design Decisions Log

| Decision | Rationale |
|----------|-----------|
| **Server-authoritative by default** | Follows Unreal/Godot best practice; prevents most client-side cheating. |
| **ECS-centric replication** | Campello uses ECS, not OOP actors. Components replicate, not objects. |
| **Transport abstraction** | Like Unity Transport + Godot MultiplayerPeer; enables WebSocket, relays, consoles. |
| **BitStream serialization** | Minimal wire size; delta compression is essential for >100 entities. |
| **Fixed network tick** | Deterministic snapshots, easier reconciliation, stable bandwidth usage. |
| **Interest management** | Required for open-world/MMO-style games; Unity NGO lacks this at scale. |
| **C++20 concepts for RPCs** | Type safety without code generation; compile-time errors for bad signatures. |
| **IPv6 dual-stack (AF_INET6)** | Single code path for v4/v6; v4 addresses stored as v4-mapped v6 (`::ffff:x.x.x.x`). |
| **Web support removed** | Simplifies transport layer; no WebSocket or WebRTC needed. |

---

## Milestones Summary

| Milestone | Phases | Target | Status |
|-----------|--------|--------|--------|
| **M0: Alpha Core** | 0–4 | Stable connections, messages, basic server/client/host. | ✅ Complete |
| **M1: Beta Sync** | 5–8 | Entity spawn, component replication, RPCs, relevancy. | ✅ Complete |
| **M2: Beta Gameplay** | 9–12 | Smooth interpolation, client prediction, clock sync, LAN discovery. | ✅ Complete |
| **M3: Multi-Transport & Security** | 13–14 | Multi-transport binding, rate limiting, encryption, profiling. | 🚧 Partial |
| **M3a: Multi-Transport Core** | 14 (partial) | `set_transport()`, `add_transport()`, cross-transport routing. | ✅ Complete |
| **M3b: Loopback Transport** | 14 (partial) | In-memory transport for instant tests and local multiplayer reference. | ✅ Complete |
| **M3c: Rate Limiting** | 13 (partial) | Token-bucket per client, max packet size, max clients. | ✅ Complete |
| **M4: Release** | 15–16 | Tooling, docs, examples, production hardening. | 🚧 Not started |

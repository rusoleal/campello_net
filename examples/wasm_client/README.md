# WASM Client Example

A minimal browser-based client using `EmscriptenWebSocketTransport` to connect to a WebSocket server via `campello_net`.

## Prerequisites

- **Emscripten** SDK installed and activated (`emcc` in PATH)
- **Python 3.7+** with `websockets` (`pip install websockets`)
- A modern browser (Chrome, Firefox, Edge, Safari)

## Build

```bash
# From the project root
emcmake cmake -B build-wasm -DCAMPELLO_NET_BUILD_EXAMPLES=ON -DCAMPELLO_NET_BUILD_TESTS=OFF
emmake cmake --build build-wasm --parallel
```

The build produces:
- `build-wasm/bin/wasm_client/wasm_client_example.js`
- `build-wasm/bin/wasm_client/wasm_client_example.wasm`

## Run

### 1. Start the WebSocket echo server

```bash
cd examples/wasm_client
python3 server.py
```

You should see:
```
WebSocket echo server listening on ws://localhost:8765
```

### 2. Serve the HTML + WASM files

You must serve the files over HTTP (browsers block WebAssembly from `file://` URLs).

```bash
cd build-wasm/bin/wasm_client
python3 -m http.server 8080
```

### 3. Open in browser

Navigate to http://localhost:8080/index.html

You should see:
- A **Connect** button — click it to open a WebSocket to `ws://localhost:8765`
- A **Send** button — type a message and send it
- The server replies with `Echo: <your message>`

## Architecture Notes

- The C++ side uses `emscripten_set_main_loop()` to run `NetworkManager::poll()` at 60 FPS via `requestAnimationFrame`.
- `EMSCRIPTEN_KEEPALIVE` functions (`connect_to_server`, `send_message`, `disconnect`) are called from JavaScript.
- Because WebSocket is TCP-based, all messages are reliable-ordered regardless of the `PacketReliability` argument.

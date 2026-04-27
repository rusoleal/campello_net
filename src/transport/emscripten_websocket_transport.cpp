#include "campello_net/transport/emscripten_websocket_transport.hpp"

#ifdef CAMPELLO_NET_PLATFORM_WASM

#include <emscripten.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace systems::leal::campello_net::transport {

// ── JS interop (EM_JS) ──────────────────────────────────────────────────────

EM_JS(int, ws_connect, (const char* url), {
    if (!Module.campello_ws_sockets) {
        Module.campello_ws_sockets = {};
        Module.campello_ws_recv_queue = {};
        Module.campello_ws_next_id = 1;
    }
    var id = Module.campello_ws_next_id++;
    var URL = UTF8ToString(url);
    var ws = new WebSocket(URL);
    ws.binaryType = 'arraybuffer';
    ws.onmessage = function(event) {
        if (event.data instanceof ArrayBuffer) {
            var arr = new Uint8Array(event.data);
            var copy = new Uint8Array(arr.length);
            copy.set(arr);
            if (!Module.campello_ws_recv_queue[id]) {
                Module.campello_ws_recv_queue[id] = [];
            }
            Module.campello_ws_recv_queue[id].push(copy);
        }
    };
    Module.campello_ws_sockets[id] = ws;
    return id;
});

EM_JS(void, ws_close, (int handle), {
    var ws = Module.campello_ws_sockets[handle];
    if (ws) {
        ws.close();
        delete Module.campello_ws_sockets[handle];
        delete Module.campello_ws_recv_queue[handle];
    }
});

EM_JS(int, ws_ready_state, (int handle), {
    var ws = Module.campello_ws_sockets[handle];
    return ws ? ws.readyState : 3; // 3 = CLOSED
});

EM_JS(void, ws_send, (int handle, const uint8_t* data, size_t len), {
    var ws = Module.campello_ws_sockets[handle];
    if (!ws || ws.readyState !== 1) return;
    var buf = new Uint8Array(len);
    buf.set(HEAPU8.subarray(data, data + len));
    ws.send(buf);
});

EM_JS(int, ws_peek_message_length, (int handle), {
    var queue = Module.campello_ws_recv_queue[handle];
    if (!queue || queue.length === 0) return -1;
    return queue[0].length;
});

EM_JS(int, ws_pop_message, (int handle, uint8_t* buffer, size_t max_len), {
    var queue = Module.campello_ws_recv_queue[handle];
    if (!queue || queue.length === 0) return -1;
    var msg = queue.shift();
    var len = Math.min(msg.length, max_len);
    HEAPU8.set(msg.subarray(0, len), buffer);
    return len;
});

// ── EmscriptenWebSocketTransport::Impl ──────────────────────────────────────

struct EmscriptenWebSocketTransport::Impl {
    int handle = 0;
    Address server_addr;
    bool connecting = false;
    bool connected = false;

    struct Packet {
        Address sender;
        std::vector<std::uint8_t> data;
    };
    std::vector<Packet> recv_queue;
    std::size_t recv_read_idx = 0;
};

// ── Public API ──────────────────────────────────────────────────────────────

EmscriptenWebSocketTransport::EmscriptenWebSocketTransport() : impl_(std::make_unique<Impl>()) {}

EmscriptenWebSocketTransport::~EmscriptenWebSocketTransport() {
    disconnect();
}

EmscriptenWebSocketTransport::EmscriptenWebSocketTransport(EmscriptenWebSocketTransport&&) noexcept = default;
EmscriptenWebSocketTransport& EmscriptenWebSocketTransport::operator=(EmscriptenWebSocketTransport&&) noexcept = default;

bool EmscriptenWebSocketTransport::bind(const Address& /*address*/) {
    // Browser WASM cannot listen for incoming connections.
    return false;
}

bool EmscriptenWebSocketTransport::connect(const Address& address) {
    disconnect();

    std::string url = "ws://" + address.ip() + ":" + std::to_string(address.port());
    impl_->handle = ws_connect(url.c_str());
    if (impl_->handle <= 0) {
        return false;
    }
    impl_->server_addr = address;
    impl_->connecting = true;
    impl_->connected = false;
    return true;
}

void EmscriptenWebSocketTransport::disconnect() {
    if (impl_->handle != 0) {
        ws_close(impl_->handle);
        impl_->handle = 0;
    }
    impl_->connecting = false;
    impl_->connected = false;
    impl_->recv_queue.clear();
    impl_->recv_read_idx = 0;
}

bool EmscriptenWebSocketTransport::is_connected() const noexcept {
    return impl_->connected;
}

bool EmscriptenWebSocketTransport::send(const std::uint8_t* data, std::size_t length,
                                        PacketReliability /*reliability*/) {
    if (!impl_->connected || impl_->handle == 0) {
        return false;
    }
    ws_send(impl_->handle, data, length);
    return true;
}

void EmscriptenWebSocketTransport::poll() {
    if (impl_->handle == 0) {
        return;
    }

    int ready = ws_ready_state(impl_->handle);
    if (ready == 1) { // OPEN
        if (impl_->connecting) {
            impl_->connecting = false;
            impl_->connected = true;
        }
    } else if (ready == 3) { // CLOSED
        if (impl_->connected || impl_->connecting) {
            impl_->connected = false;
            impl_->connecting = false;
        }
    }

    if (!impl_->connected) {
        return;
    }

    // Drain JS-side message queue into C++ queue
    while (true) {
        int len = ws_peek_message_length(impl_->handle);
        if (len < 0) {
            break;
        }
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(len));
        int copied = ws_pop_message(impl_->handle, buf.data(), buf.size());
        if (copied > 0) {
            buf.resize(static_cast<std::size_t>(copied));
            impl_->recv_queue.push_back({impl_->server_addr, std::move(buf)});
        } else {
            break;
        }
    }
}

bool EmscriptenWebSocketTransport::pop_receive(std::uint8_t* buffer, std::size_t max_length,
                                               std::size_t& out_length, Address& out_sender) {
    if (impl_->recv_read_idx >= impl_->recv_queue.size()) {
        impl_->recv_queue.clear();
        impl_->recv_read_idx = 0;
        return false;
    }

    auto& pkt = impl_->recv_queue[impl_->recv_read_idx++];
    out_length = std::min(max_length, pkt.data.size());
    std::memcpy(buffer, pkt.data.data(), out_length);
    out_sender = pkt.sender;
    return true;
}

float EmscriptenWebSocketTransport::rtt() const noexcept {
    // WebSocket does not expose per-message RTT. Applications should measure
    // RTT via application-level pings or NetworkTime samples.
    return 0.0f;
}

float EmscriptenWebSocketTransport::packet_loss() const noexcept {
    // TCP transports do not expose packet loss ratios.
    return 0.0f;
}

} // namespace systems::leal::campello_net::transport

#endif // CAMPELLO_NET_PLATFORM_WASM

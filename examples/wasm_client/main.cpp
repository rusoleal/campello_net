#include <campello_net/network_log.hpp>
#include <campello_net/network_manager.hpp>
#include <campello_net/transport/address.hpp>
#include <campello_net/transport/emscripten_websocket_transport.hpp>
#include <campello_net/transport/packet.hpp>
#include <cstdint>
#include <cstring>
#include <emscripten.h>
#include <string>

using namespace systems::leal::campello_net;

static NetworkManager* g_net = nullptr;

extern "C" {

EMSCRIPTEN_KEEPALIVE
bool connect_to_server(const char* host, int port) {
    if (!g_net)
        return false;

    auto transport = std::make_unique<transport::EmscriptenWebSocketTransport>();
    g_net->set_transport(std::move(transport));

    NetworkManager::Config config;
    config.mode = NetworkManager::Mode::Client;
    config.server_address = transport::Address(std::string(host), static_cast<std::uint16_t>(port));
    return g_net->start(config);
}

EMSCRIPTEN_KEEPALIVE
void send_message(const char* text) {
    if (!g_net)
        return;
    g_net->send(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text),
                transport::PacketReliability::ReliableOrdered);
}

EMSCRIPTEN_KEEPALIVE
void disconnect() {
    if (!g_net)
        return;
    g_net->disconnect();
}

EMSCRIPTEN_KEEPALIVE
bool is_connected() {
    if (!g_net)
        return false;
    return g_net->is_active() && g_net->local_client_id() != 0;
}

} // extern "C"

static void poll_loop() {
    if (!g_net)
        return;

    g_net->poll();

    // Drain received messages and push them to the HTML log
    NetworkManager::ReceivedMessage msg;
    while (g_net->pop_message(msg)) {
        std::string payload(reinterpret_cast<const char*>(msg.payload.data()), msg.payload.size());
        EM_ASM(
            {
                var text = UTF8ToString($0);
                console.log("[WASM] Received: " + text);
                var log = document.getElementById('log');
                if (log) {
                    var entry = document.createElement('div');
                    entry.textContent = '\u2190 ' + text;
                    entry.style.color = '#0f0';
                    log.appendChild(entry);
                    log.scrollTop = log.scrollHeight;
                }
            },
            payload.c_str());
    }

    // Update UI connection status
    bool connected = is_connected();
    EM_ASM(
        {
            var status = document.getElementById('status');
            if (status) {
                status.textContent = $0 ? 'Connected (id=' + $1 + ')' : 'Connecting...';
                status.style.color = $0 ? '#0f0' : '#ff0';
            }
        },
        connected, static_cast<int>(g_net->local_client_id()));
}

int main() {
    static NetworkManager net;
    g_net = &net;

    network_log::set_log_callback([](network_log::LogLevel level, const std::string& message) {
        const char* prefix = (level == network_log::LogLevel::Error)     ? "[ERR]"
                             : (level == network_log::LogLevel::Warning) ? "[WRN]"
                                                                         : "[INF]";
        EM_ASM({ console.log(UTF8ToString($0) + " " + UTF8ToString($1)); }, prefix, message.c_str());
    });

    emscripten_set_main_loop(poll_loop, 0, 1);
    return 0;
}

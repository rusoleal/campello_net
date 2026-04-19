#include "campello_net/network_manager.hpp"

#include "campello_net/network_entity.hpp"
#include "campello_net/network_log.hpp"
#include "campello_net/network_replication.hpp"
#include "campello_net/rate_limiter.hpp"
#include "campello_net/rpc_manager.hpp"
#include "campello_net/transport/udp_transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

namespace systems::leal::campello_net {

// ── Endian helpers (system messages always big-endian on wire) ──────────────

namespace {

inline std::uint64_t bswap64(std::uint64_t x) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_bswap64(x);
#elif defined(_MSC_VER)
    return _byteswap_uint64(x);
#else
    return ((x & 0xFF00000000000000ULL) >> 56) | ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0x0000FF0000000000ULL) >> 24) | ((x & 0x000000FF00000000ULL) >> 8) |
           ((x & 0x00000000FF000000ULL) << 8) | ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x000000000000FF00ULL) << 40) | ((x & 0x00000000000000FFULL) << 56);
#endif
}

void write_u64_be(std::uint8_t* dst, std::uint64_t value) noexcept {
    std::uint64_t be = bswap64(value);
    std::memcpy(dst, &be, 8);
}

std::uint64_t read_u64_be(const std::uint8_t* src) noexcept {
    std::uint64_t be;
    std::memcpy(&be, src, 8);
    return bswap64(be);
}

void write_double_be(std::uint8_t* dst, double value) noexcept {
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(value));
    write_u64_be(dst, bits);
}

double read_double_be(const std::uint8_t* src) noexcept {
    std::uint64_t bits = read_u64_be(src);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] double now_seconds() {
    auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

} // anonymous namespace

// ── Impl definition ─────────────────────────────────────────────────────────

struct NetworkManager::Impl {
    Mode mode = Mode::None;
    Config config{};
    std::unique_ptr<transport::ITransport> transport;
    std::vector<std::unique_ptr<transport::ITransport>> additional_transports;

    struct ClientEntry {
        ClientId id = 0;
        transport::Address address;
        transport::ITransport* transport = nullptr; ///< nullptr = local client
        bool approved = false;
        bool disconnecting = false;
        RateLimiter rate_limiter;

        // Stats tracking
        std::uint64_t bytes_sent = 0;
        std::uint64_t bytes_received = 0;
        std::uint64_t packets_sent = 0;
        std::uint64_t packets_received = 0;
        std::uint64_t last_bytes_sent = 0;
        std::uint64_t last_bytes_received = 0;
        double last_stats_update = 0.0;
        float bandwidth_in = 0.0f;
        float bandwidth_out = 0.0f;
    };
    std::vector<ClientEntry> clients;

    std::vector<ReceivedMessage> message_queue;
    std::size_t message_read_idx = 0;

    ClientCallback on_connected;
    ClientCallback on_disconnected;
    DataCallback on_data;
    ApprovalCallback on_approval;

    // Client state
    bool client_connected = false;
    ClientId local_client_id = 0;
    NetworkTime net_time;
    double last_time_sync = 0.0;
    bool awaiting_accept = false;

    // Host mode loopback
    std::vector<ReceivedMessage> host_local_recv_queue;

    enum class SysType : std::uint8_t {
        ConnectRequest = 0,
        ConnectAccept = 1,
        ConnectReject = 2,
        DisconnectNotify = 3,
        TimeSyncRequest = 4,
        TimeSyncResponse = 5,
        EntitySpawn = 0x10,
        EntityDestroy = 0x11,
        EntitySetOwner = 0x12,
        EntityFullState = 0x13,
        DeltaState = 0x20,
        SnapshotAck = 0x21,
        Rpc = 0x22,
    };

    // ── Helpers ──

    ClientEntry* find_client_by_id(ClientId id) noexcept {
        for (auto& c : clients) {
            if (c.id == id)
                return &c;
        }
        return nullptr;
    }

    ClientEntry* find_client_by_address(const transport::Address& addr) noexcept {
        for (auto& c : clients) {
            if (c.address == addr)
                return &c;
        }
        return nullptr;
    }

    ClientId generate_client_id() noexcept {
        ClientId id = 1;
        while (find_client_by_id(id) != nullptr) {
            ++id;
        }
        return id;
    }

    void push_message(ClientId client, std::vector<std::uint8_t> payload) {
        ReceivedMessage msg;
        msg.client = client;
        msg.payload = std::move(payload);
        message_queue.push_back(std::move(msg));
    }

    void push_message_to_host_local(ClientId client, std::vector<std::uint8_t> payload) {
        ReceivedMessage msg;
        msg.client = client;
        msg.payload = std::move(payload);
        host_local_recv_queue.push_back(std::move(msg));
    }

    void flush_to_callbacks(ClientId client, const std::uint8_t* data, std::size_t len) {
        if (on_data) {
            on_data(client, data, len);
        }
    }

    static constexpr std::array<std::uint8_t, 2> SYS_MAGIC = {0xCA, 0xFE};

    void send_sys(transport::ITransport* t, const transport::Address& to, SysType type, const std::uint8_t* payload,
                  std::size_t len) {
        if (!t)
            return;
        std::vector<std::uint8_t> packet(2 + 1 + len);
        packet[0] = SYS_MAGIC[0];
        packet[1] = SYS_MAGIC[1];
        packet[2] = static_cast<std::uint8_t>(type);
        if (len > 0) {
            std::memcpy(packet.data() + 3, payload, len);
        }
        t->send_to(to, packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
    }

    void broadcast_sys(SysType type, const std::uint8_t* payload, std::size_t len, ClientId exclude = 0) {
        for (auto& c : clients) {
            if (c.id == exclude)
                continue;
            if (!c.approved)
                continue;
            if (!c.transport)
                continue; // local clients don't need network sys messages
            send_sys(c.transport, c.address, type, payload, len);
        }
    }

    void process_system_message(ClientId sender_id, const transport::Address& sender_addr,
                                transport::ITransport* from_transport, SysType type, const std::uint8_t* data,
                                std::size_t len);

    void poll_transport_receives_for(transport::ITransport* t);
    void poll_transport_receives();
    void poll_client_time_sync();
    void send_connect_request();
    void disconnect_transport_client(transport::ITransport* t, const transport::Address& addr);

    class NetworkEntityManager* entity_manager = nullptr;
    class NetworkReplicationManager* replication_manager = nullptr;
    class RpcManager* rpc_manager = nullptr;
};

// ── System message processing ───────────────────────────────────────────────

void NetworkManager::Impl::process_system_message(ClientId sender_id, const transport::Address& sender_addr,
                                                  transport::ITransport* from_transport, SysType type,
                                                  const std::uint8_t* data, std::size_t len) {
    switch (type) {
    case SysType::ConnectRequest: {
        // Server / Host only
        if (mode != Mode::Server && mode != Mode::Host)
            return;

        auto* existing = find_client_by_address(sender_addr);
        if (existing && existing->approved) {
            // Already approved, resend accept
            std::uint8_t buf[8];
            write_u64_be(buf, existing->id);
            send_sys(from_transport, sender_addr, SysType::ConnectAccept, buf, 8);
            return;
        }

        bool approved = true;
        std::vector<std::uint8_t> auth_data;
        if (len > 0) {
            auth_data.assign(data, data + len);
        }
        if (on_approval) {
            approved = on_approval(sender_addr, auth_data);
        }

        if (approved) {
            // Enforce max_clients
            std::size_t approved_count = 0;
            for (const auto& c : clients) {
                if (c.approved)
                    ++approved_count;
            }
            if (approved_count >= config.max_clients) {
                std::vector<std::uint8_t> reason = {'F', 'U', 'L', 'L'};
                send_sys(from_transport, sender_addr, SysType::ConnectReject, reason.data(), reason.size());
                disconnect_transport_client(from_transport, sender_addr);
                break;
            }

            ClientId new_id = generate_client_id();
            ClientEntry entry;
            entry.id = new_id;
            entry.address = sender_addr;
            entry.transport = from_transport;
            entry.approved = true;
            entry.rate_limiter.configure_messages(config.max_messages_per_sec, config.rate_limit_burst);
            entry.rate_limiter.configure_bytes(config.max_bytes_per_sec, config.rate_limit_burst * 1024.0f);
            entry.rate_limiter.configure_rpcs(config.max_rpcs_per_sec, config.rate_limit_burst);
            clients.push_back(std::move(entry));

            std::uint8_t buf[8];
            write_u64_be(buf, new_id);
            send_sys(from_transport, sender_addr, SysType::ConnectAccept, buf, 8);

            CAMPELLO_NET_LOGI("Client " + std::to_string(new_id) + " connected from " + sender_addr.to_string());
            if (on_connected) {
                on_connected(new_id);
            }
            if (replication_manager) {
                replication_manager->on_client_connected(new_id);
            }
        } else {
            std::vector<std::uint8_t> reason = {'R', 'J', 'C', 'T'};
            send_sys(from_transport, sender_addr, SysType::ConnectReject, reason.data(), reason.size());
            // Give transport a moment to send reject, then disconnect
            disconnect_transport_client(from_transport, sender_addr);
        }
        break;
    }

    case SysType::ConnectAccept: {
        // Client only
        if (mode != Mode::Client && mode != Mode::Host)
            return;
        if (len < 8)
            return;

        local_client_id = read_u64_be(data);
        client_connected = true;
        awaiting_accept = false;

        if (on_connected) {
            on_connected(local_client_id);
        }
        if (replication_manager) {
            replication_manager->on_client_connected(local_client_id);
        }
        break;
    }

    case SysType::ConnectReject: {
        // Client only
        if (mode != Mode::Client && mode != Mode::Host)
            return;
        awaiting_accept = false;
        if (transport) {
            transport->disconnect();
        }
        client_connected = false;
        break;
    }

    case SysType::DisconnectNotify: {
        if (mode == Mode::Server || mode == Mode::Host) {
            auto* entry = find_client_by_address(sender_addr);
            if (entry) {
                ClientId id = entry->id;
                CAMPELLO_NET_LOGI("Client " + std::to_string(id) + " disconnected");
                clients.erase(std::remove_if(clients.begin(), clients.end(),
                                             [id](const ClientEntry& c) {
                                                 return c.id == id;
                                             }),
                              clients.end());
                if (on_disconnected) {
                    on_disconnected(id);
                }
                if (replication_manager) {
                    replication_manager->on_client_disconnected(id);
                }
            }
        } else if (mode == Mode::Client) {
            client_connected = false;
            local_client_id = 0;
            if (transport) {
                transport->disconnect();
            }
            if (on_disconnected) {
                on_disconnected(0);
            }
            if (replication_manager) {
                replication_manager->on_client_disconnected(0);
            }
        }
        break;
    }

    case SysType::TimeSyncRequest: {
        // Server / Host only
        if (mode != Mode::Server && mode != Mode::Host)
            return;
        if (len < 8)
            return;
        double t1 = now_seconds();
        double t0 = read_double_be(data);
        double t2 = now_seconds();

        std::array<std::uint8_t, 24> buf{};
        write_double_be(buf.data(), t0);
        write_double_be(buf.data() + 8, t1);
        write_double_be(buf.data() + 16, t2);
        send_sys(from_transport, sender_addr, SysType::TimeSyncResponse, buf.data(), buf.size());
        break;
    }

    case SysType::TimeSyncResponse: {
        // Client only
        if (mode != Mode::Client && mode != Mode::Host)
            return;
        if (len < 24)
            return;
        double t0 = read_double_be(data);
        double t1 = read_double_be(data + 8);
        double t2 = read_double_be(data + 16);
        double t3 = now_seconds();
        net_time.record_sample(t0, t1, t2, t3);
        break;
    }

    case SysType::EntitySpawn: {
        if (entity_manager) {
            entity_manager->on_receive_spawn(sender_id, data, len);
        }
        break;
    }
    case SysType::EntityDestroy: {
        if (entity_manager) {
            entity_manager->on_receive_destroy(sender_id, data, len);
        }
        break;
    }
    case SysType::EntitySetOwner: {
        if (entity_manager) {
            entity_manager->on_receive_set_owner(sender_id, data, len);
        }
        break;
    }
    case SysType::EntityFullState: {
        if (entity_manager) {
            entity_manager->on_receive_full_state(sender_id, data, len);
        }
        break;
    }
    case SysType::DeltaState: {
        if (replication_manager) {
            replication_manager->on_receive_delta(data, len);
        }
        break;
    }
    case SysType::SnapshotAck: {
        if (replication_manager && len >= 2) {
            std::uint16_t ack_id = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
            replication_manager->on_snapshot_ack(sender_id, ack_id);
        }
        break;
    }
    case SysType::Rpc: {
        if (rpc_manager) {
            if (mode == Mode::Server || mode == Mode::Host) {
                auto* entry = find_client_by_address(sender_addr);
                if (entry && !entry->rate_limiter.allow_rpc()) {
                    break; // drop exceeded RPC
                }
            }
            rpc_manager->on_receive(sender_id, data, len);
        }
        break;
    }
    }
}

// ── Transport receive polling ───────────────────────────────────────────────

void NetworkManager::Impl::poll_transport_receives_for(transport::ITransport* t) {
    if (!t)
        return;

    constexpr std::size_t BUF_SIZE = 4096;
    std::array<std::uint8_t, BUF_SIZE> buffer{};

    while (true) {
        std::size_t len = 0;
        transport::Address sender;
        if (!t->pop_receive(buffer.data(), buffer.size(), len, sender)) {
            break;
        }
        if (len == 0)
            continue;

        // Enforce max_packet_size on all inbound packets
        if (len > config.max_packet_size) {
            if (mode == Mode::Server || mode == Mode::Host) {
                auto* entry = find_client_by_address(sender);
                if (entry && entry->id != local_client_id && entry->transport) {
                    CAMPELLO_NET_LOGW("Oversized packet (" + std::to_string(len) + " bytes) from client " +
                                      std::to_string(entry->id) + ", disconnecting");
                    disconnect_transport_client(entry->transport, entry->address);
                    ClientId id = entry->id;
                    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                                 [id](const ClientEntry& c) {
                                                     return c.id == id;
                                                 }),
                                  clients.end());
                    if (on_disconnected)
                        on_disconnected(id);
                    if (replication_manager)
                        replication_manager->on_client_disconnected(id);
                }
            }
            continue;
        }

        // Check for system message magic prefix [0xCA][0xFE]
        if (len >= 3 && buffer[0] == SYS_MAGIC[0] && buffer[1] == SYS_MAGIC[1]) {
            auto sys_type = static_cast<SysType>(buffer[2]);
            const std::uint8_t* payload = buffer.data() + 3;
            std::size_t payload_len = (len > 3) ? (len - 3) : 0;

            ClientId sender_id = 0;
            if (mode == Mode::Server || mode == Mode::Host) {
                auto* entry = find_client_by_address(sender);
                if (entry) {
                    sender_id = entry->id;
                } else if (sys_type != SysType::ConnectRequest) {
                    continue; // unknown client
                }
            }
            process_system_message(sender_id, sender, t, sys_type, payload, payload_len);
            continue;
        }

        // User data
        ClientId sender_id = 0;
        if (mode == Mode::Server || mode == Mode::Host) {
            auto* entry = find_client_by_address(sender);
            if (entry) {
                sender_id = entry->id;
            } else {
                continue; // unknown client
            }
            // Rate-limit inbound user data
            if (!entry->rate_limiter.allow_message(len)) {
                CAMPELLO_NET_LOGV("Rate limit dropped message from client " + std::to_string(sender_id));
                continue; // silently drop
            }
            entry->bytes_received += len;
            ++entry->packets_received;
            std::vector<std::uint8_t> data(buffer.data(), buffer.data() + len);
            push_message(sender_id, std::move(data));
            flush_to_callbacks(sender_id, buffer.data(), len);
        } else if (mode == Mode::Client) {
            std::vector<std::uint8_t> data(buffer.data(), buffer.data() + len);
            push_message(0, std::move(data));
            flush_to_callbacks(0, buffer.data(), len);
        }
    }
}

void NetworkManager::Impl::poll_transport_receives() {
    if (transport) {
        poll_transport_receives_for(transport.get());
    }
    for (auto& extra : additional_transports) {
        if (extra) {
            poll_transport_receives_for(extra.get());
        }
    }
}

// ── Client time sync ────────────────────────────────────────────────────────

void NetworkManager::Impl::poll_client_time_sync() {
    if ((mode != Mode::Client && mode != Mode::Host) || !client_connected)
        return;

    double t = now_seconds();
    if (last_time_sync == 0.0) {
        last_time_sync = t; // defer first sync until next interval
        return;
    }
    if (t - last_time_sync > 2.0) {
        last_time_sync = t;
        std::array<std::uint8_t, 8> buf{};
        write_double_be(buf.data(), t);
        if (mode == Mode::Host) {
            // Host mode: process time sync locally
            double t1 = t;
            double t2 = t;
            double t3 = t;
            net_time.record_sample(t, t1, t2, t3);
        } else if (transport) {
            std::vector<std::uint8_t> packet(2 + 1 + 8);
            packet[0] = SYS_MAGIC[0];
            packet[1] = SYS_MAGIC[1];
            packet[2] = static_cast<std::uint8_t>(SysType::TimeSyncRequest);
            std::memcpy(packet.data() + 3, buf.data(), 8);
            transport->send(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
        }
    }
}

// ── Connect request ─────────────────────────────────────────────────────────

void NetworkManager::Impl::send_connect_request() {
    if (!transport)
        return;
    std::array<std::uint8_t, 3> packet{SYS_MAGIC[0], SYS_MAGIC[1], static_cast<std::uint8_t>(SysType::ConnectRequest)};
    transport->send(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
    awaiting_accept = true;
}

// ── Disconnect transport client ─────────────────────────────────────────────

void NetworkManager::Impl::disconnect_transport_client(transport::ITransport* t, const transport::Address& addr) {
    if (!t)
        return;
    std::array<std::uint8_t, 3> packet{SYS_MAGIC[0], SYS_MAGIC[1],
                                       static_cast<std::uint8_t>(SysType::DisconnectNotify)};
    t->send_to(addr, packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
}

// ── NetworkManager public methods ───────────────────────────────────────────

NetworkManager::NetworkManager() : impl_(std::make_unique<Impl>()) {}

NetworkManager::~NetworkManager() = default;

NetworkManager::NetworkManager(NetworkManager&&) noexcept = default;

NetworkManager& NetworkManager::operator=(NetworkManager&&) noexcept = default;

void NetworkManager::set_transport(std::unique_ptr<transport::ITransport> transport) {
    if (is_active())
        stop();
    impl_->transport = std::move(transport);
}

void NetworkManager::add_transport(std::unique_ptr<transport::ITransport> transport) {
    if (transport) {
        impl_->additional_transports.push_back(std::move(transport));
    }
}

bool NetworkManager::start(const Config& config) {
    if (impl_->mode != Mode::None) {
        stop();
    }

    impl_->config = config;
    impl_->mode = config.mode;

    // Use user-provided transport or create a default UdpTransport
    if (!impl_->transport) {
        impl_->transport = std::make_unique<transport::UdpTransport>();
    }

    if (config.mode == Mode::Server || config.mode == Mode::Host) {
        if (!impl_->transport->bind(config.bind_address)) {
            impl_->mode = Mode::None;
            impl_->transport.reset();
            return false;
        }
    }

    if (config.mode == Mode::Client) {
        if (!impl_->transport->connect(config.server_address)) {
            impl_->mode = Mode::None;
            impl_->transport.reset();
            return false;
        }
        impl_->awaiting_accept = true;
        impl_->send_connect_request();
    }

    if (config.mode == Mode::Host) {
        // Host mode: create local client entry
        ClientId local_id = impl_->generate_client_id();
        Impl::ClientEntry entry;
        entry.id = local_id;
        entry.approved = true;
        impl_->clients.push_back(std::move(entry));
        impl_->local_client_id = local_id;
        impl_->client_connected = true;
        if (impl_->on_connected) {
            impl_->on_connected(local_id);
        }
        CAMPELLO_NET_LOGI("Host started on " + config.bind_address.to_string());
    } else if (config.mode == Mode::Server) {
        CAMPELLO_NET_LOGI("Server started on " + config.bind_address.to_string());
    } else if (config.mode == Mode::Client) {
        CAMPELLO_NET_LOGI("Client connecting to " + config.server_address.to_string());
    }

    return true;
}

void NetworkManager::stop() {
    CAMPELLO_NET_LOGI("Stopping network manager");

    std::array<std::uint8_t, 3> packet{Impl::SYS_MAGIC[0], Impl::SYS_MAGIC[1],
                                       static_cast<std::uint8_t>(Impl::SysType::DisconnectNotify)};

    if (impl_->mode == Mode::Server || impl_->mode == Mode::Host) {
        for (auto& c : impl_->clients) {
            if (c.approved && c.id != impl_->local_client_id && c.transport) {
                c.transport->send_to(c.address, packet.data(), packet.size(),
                                     transport::PacketReliability::ReliableOrdered);
            }
        }
    } else if (impl_->mode == Mode::Client && impl_->client_connected) {
        if (impl_->transport) {
            impl_->transport->send(packet.data(), packet.size(), transport::PacketReliability::ReliableOrdered);
        }
    }

    // Give the OS a moment to actually transmit the UDP packet before
    // closing the socket. Without this, the disconnect notify can be
    // dropped under load (observed on macOS).
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (impl_->transport) {
        impl_->transport->disconnect();
    }
    for (auto& t : impl_->additional_transports) {
        if (t)
            t->disconnect();
    }

    impl_->clients.clear();
    impl_->message_queue.clear();
    impl_->message_read_idx = 0;
    impl_->host_local_recv_queue.clear();
    impl_->client_connected = false;
    impl_->local_client_id = 0;
    impl_->awaiting_accept = false;
    impl_->net_time.reset();
    impl_->mode = Mode::None;
    impl_->transport.reset();
    impl_->additional_transports.clear();
}

void NetworkManager::poll() {
    if (!impl_->transport && impl_->mode != Mode::Host && impl_->additional_transports.empty())
        return;

    if (impl_->transport) {
        impl_->transport->poll();
    }
    for (auto& t : impl_->additional_transports) {
        if (t)
            t->poll();
    }

    impl_->poll_transport_receives();
    impl_->poll_client_time_sync();

    // Update bandwidth estimates for all clients
    double now = now_seconds();
    for (auto& c : impl_->clients) {
        if (!c.approved)
            continue;
        if (c.last_stats_update > 0.0) {
            double dt = now - c.last_stats_update;
            if (dt > 0.0) {
                float instant_in = static_cast<float>((c.bytes_received - c.last_bytes_received) / dt);
                float instant_out = static_cast<float>((c.bytes_sent - c.last_bytes_sent) / dt);
                c.bandwidth_in = c.bandwidth_in * 0.7f + instant_in * 0.3f;
                c.bandwidth_out = c.bandwidth_out * 0.7f + instant_out * 0.3f;
            }
        }
        c.last_bytes_received = c.bytes_received;
        c.last_bytes_sent = c.bytes_sent;
        c.last_stats_update = now;
    }

    // Client mode: check if transport connected and send connect request
    if (impl_->mode == Mode::Client && !impl_->client_connected && !impl_->awaiting_accept) {
        if (impl_->transport && impl_->transport->is_connected()) {
            impl_->send_connect_request();
        }
    }

    // Host mode: deliver loopback messages
    if (impl_->mode == Mode::Host) {
        for (auto& msg : impl_->host_local_recv_queue) {
            impl_->message_queue.push_back(std::move(msg));
        }
        impl_->host_local_recv_queue.clear();
    }
}

// ── Local clients ───────────────────────────────────────────────────────────

ClientId NetworkManager::add_local_client() {
    if (impl_->mode != Mode::Server && impl_->mode != Mode::Host) {
        return 0;
    }

    ClientId new_id = impl_->generate_client_id();
    Impl::ClientEntry entry;
    entry.id = new_id;
    entry.approved = true;
    entry.transport = nullptr; // local
    impl_->clients.push_back(std::move(entry));

    if (impl_->on_connected) {
        impl_->on_connected(new_id);
    }
    if (impl_->replication_manager) {
        impl_->replication_manager->on_client_connected(new_id);
    }

    return new_id;
}

void NetworkManager::remove_local_client(ClientId client) {
    disconnect_client(client);
}

// ── Sending ─────────────────────────────────────────────────────────────────

bool NetworkManager::send(ClientId client, const std::uint8_t* data, std::size_t length,
                          transport::PacketReliability reliability) {
    if (impl_->mode != Mode::Server && impl_->mode != Mode::Host)
        return false;

    // Host mode loopback
    if (impl_->mode == Mode::Host && client == impl_->local_client_id) {
        std::vector<std::uint8_t> copy(data, data + length);
        impl_->push_message_to_host_local(client, std::move(copy));
        return true;
    }

    auto* entry = impl_->find_client_by_id(client);
    if (!entry || !entry->approved)
        return false;
    if (!entry->transport) {
        // Local client (non-host) — deliver directly
        std::vector<std::uint8_t> copy(data, data + length);
        impl_->push_message(client, std::move(copy));
        impl_->flush_to_callbacks(client, data, length);
        return true;
    }

    bool ok = entry->transport->send_to(entry->address, data, length, reliability);
    if (ok) {
        entry->bytes_sent += length;
        ++entry->packets_sent;
    }
    return ok;
}

void NetworkManager::broadcast(const std::uint8_t* data, std::size_t length, transport::PacketReliability reliability,
                               ClientId exclude) {
    if (impl_->mode != Mode::Server && impl_->mode != Mode::Host)
        return;

    // Host mode: queue to local client
    if (impl_->mode == Mode::Host && impl_->local_client_id != 0 && impl_->local_client_id != exclude) {
        std::vector<std::uint8_t> copy(data, data + length);
        ReceivedMessage msg;
        msg.client = impl_->local_client_id;
        msg.payload = std::move(copy);
        impl_->host_local_recv_queue.push_back(std::move(msg));
    }

    for (auto& c : impl_->clients) {
        if (c.id == exclude)
            continue;
        if (!c.approved)
            continue;
        if (c.id == impl_->local_client_id)
            continue; // already handled above
        if (!c.transport) {
            // Local client — deliver directly
            std::vector<std::uint8_t> copy(data, data + length);
            impl_->push_message(c.id, std::move(copy));
            impl_->flush_to_callbacks(c.id, data, length);
            continue;
        }
        bool ok = c.transport->send_to(c.address, data, length, reliability);
        if (ok) {
            c.bytes_sent += length;
            ++c.packets_sent;
        }
    }
}

bool NetworkManager::send(const std::uint8_t* data, std::size_t length, transport::PacketReliability reliability) {
    if (impl_->mode == Mode::Client) {
        if (!impl_->transport || !impl_->client_connected)
            return false;
        return impl_->transport->send(data, length, reliability);
    }
    if (impl_->mode == Mode::Host) {
        // Send as local client — deliver directly to server side
        if (impl_->local_client_id == 0)
            return false;
        std::vector<std::uint8_t> copy(data, data + length);
        ReceivedMessage msg;
        msg.client = impl_->local_client_id;
        msg.payload = std::move(copy);
        impl_->message_queue.push_back(std::move(msg));
        impl_->flush_to_callbacks(impl_->local_client_id, data, length);
        return true;
    }
    return false;
}

// ── Receiving ───────────────────────────────────────────────────────────────

bool NetworkManager::pop_message(ReceivedMessage& out_msg) {
    if (impl_->message_read_idx >= impl_->message_queue.size()) {
        impl_->message_queue.clear();
        impl_->message_read_idx = 0;
        return false;
    }
    out_msg = std::move(impl_->message_queue[impl_->message_read_idx++]);
    return true;
}

// ── Callbacks ───────────────────────────────────────────────────────────────

void NetworkManager::on_client_connected(ClientCallback cb) {
    impl_->on_connected = std::move(cb);
}

void NetworkManager::on_client_disconnected(ClientCallback cb) {
    impl_->on_disconnected = std::move(cb);
}

void NetworkManager::on_data_received(DataCallback cb) {
    impl_->on_data = std::move(cb);
}

void NetworkManager::set_connection_approval(ApprovalCallback cb) {
    impl_->on_approval = std::move(cb);
}

// ── Disconnection ───────────────────────────────────────────────────────────

void NetworkManager::disconnect_client(ClientId client) {
    if (impl_->mode != Mode::Server && impl_->mode != Mode::Host)
        return;

    auto* entry = impl_->find_client_by_id(client);
    if (!entry)
        return;

    if (client == impl_->local_client_id) {
        // Host mode local client
        impl_->client_connected = false;
        impl_->local_client_id = 0;
    } else if (entry->transport) {
        std::array<std::uint8_t, 3> packet{Impl::SYS_MAGIC[0], Impl::SYS_MAGIC[1],
                                           static_cast<std::uint8_t>(Impl::SysType::DisconnectNotify)};
        entry->transport->send_to(entry->address, packet.data(), packet.size(),
                                  transport::PacketReliability::ReliableOrdered);
    }

    impl_->clients.erase(std::remove_if(impl_->clients.begin(), impl_->clients.end(),
                                        [client](const Impl::ClientEntry& c) {
                                            return c.id == client;
                                        }),
                         impl_->clients.end());

    if (impl_->on_disconnected) {
        impl_->on_disconnected(client);
    }
    if (impl_->replication_manager) {
        impl_->replication_manager->on_client_disconnected(client);
    }
}

void NetworkManager::disconnect() {
    if (impl_->mode == Mode::Client) {
        if (impl_->transport && impl_->client_connected) {
            std::uint8_t type = static_cast<std::uint8_t>(Impl::SysType::DisconnectNotify);
            impl_->transport->send(&type, 1, transport::PacketReliability::ReliableOrdered);
        }
        stop();
    } else if (impl_->mode == Mode::Host) {
        // Disconnect local client
        if (impl_->local_client_id != 0) {
            disconnect_client(impl_->local_client_id);
        }
    }
}

// ── Queries ─────────────────────────────────────────────────────────────────

NetworkManager::Mode NetworkManager::mode() const noexcept {
    return impl_->mode;
}

bool NetworkManager::is_active() const noexcept {
    return impl_->mode != Mode::None;
}

std::size_t NetworkManager::client_count() const noexcept {
    std::size_t count = 0;
    for (auto& c : impl_->clients) {
        if (c.approved)
            ++count;
    }
    return count;
}

bool NetworkManager::is_client_connected(ClientId client) const noexcept {
    auto* entry = impl_->find_client_by_id(client);
    return entry != nullptr && entry->approved;
}

transport::Address NetworkManager::client_address(ClientId client) const {
    auto* entry = impl_->find_client_by_id(client);
    if (entry)
        return entry->address;
    return transport::Address{};
}

float NetworkManager::client_rtt(ClientId client) const noexcept {
    if (impl_->mode == Mode::Host && client == impl_->local_client_id) {
        return 0.0f;
    }
    auto* entry = impl_->find_client_by_id(client);
    if (!entry || !entry->transport)
        return 0.0f;
    return entry->transport->get_connection_rtt(entry->address);
}

float NetworkManager::client_packet_loss(ClientId client) const noexcept {
    if (impl_->mode == Mode::Host && client == impl_->local_client_id) {
        return 0.0f;
    }
    auto* entry = impl_->find_client_by_id(client);
    if (!entry || !entry->transport)
        return 0.0f;
    return entry->transport->get_connection_packet_loss(entry->address);
}

NetStats NetworkManager::net_stats(ClientId client) const noexcept {
    NetStats stats{};
    auto* entry = impl_->find_client_by_id(client);
    if (!entry)
        return stats;

    stats.bytes_sent = entry->bytes_sent;
    stats.bytes_received = entry->bytes_received;
    stats.packets_sent = entry->packets_sent;
    stats.packets_received = entry->packets_received;
    stats.bandwidth_out = entry->bandwidth_out;
    stats.bandwidth_in = entry->bandwidth_in;
    stats.rtt = client_rtt(client);
    stats.packet_loss = client_packet_loss(client);
    return stats;
}

double NetworkManager::network_time() const noexcept {
    if (impl_->mode == Mode::Host) {
        return now_seconds();
    }
    return impl_->net_time.local_to_remote(now_seconds());
}

ClientId NetworkManager::local_client_id() const noexcept {
    return impl_->local_client_id;
}

void NetworkManager::set_entity_manager(class NetworkEntityManager* mgr) noexcept {
    if (impl_)
        impl_->entity_manager = mgr;
}

void NetworkManager::set_replication_manager(class NetworkReplicationManager* mgr) noexcept {
    if (!impl_)
        return;
    impl_->replication_manager = mgr;
    if (!mgr)
        return;

    // Notify replication manager about already-connected clients.
    for (const auto& c : impl_->clients) {
        if (c.approved) {
            mgr->on_client_connected(c.id);
        }
    }
    if (impl_->mode == Mode::Host && impl_->local_client_id != 0) {
        mgr->on_client_connected(impl_->local_client_id);
    }
}

void NetworkManager::set_rpc_manager(class RpcManager* mgr) noexcept {
    if (impl_)
        impl_->rpc_manager = mgr;
}

} // namespace systems::leal::campello_net

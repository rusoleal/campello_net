#include "campello_net/transport/udp_transport.hpp"

#include "campello_net/transport/packet.hpp"

#ifdef CAMPELLO_NET_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace systems::leal::campello_net::transport {

// ── Platform helpers ────────────────────────────────────────────────────────

#ifdef CAMPELLO_NET_PLATFORM_WIN32
using SocketType = SOCKET;
constexpr SocketType INVALID_SOCKET_VAL = INVALID_SOCKET;
#else
using SocketType = int;
constexpr SocketType INVALID_SOCKET_VAL = -1;
#endif

namespace {

struct WsaLifetime {
    WsaLifetime() {
#ifdef CAMPELLO_NET_PLATFORM_WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~WsaLifetime() {
#ifdef CAMPELLO_NET_PLATFORM_WIN32
        WSACleanup();
#endif
    }
} g_wsa_lifetime;

[[nodiscard]] double now_seconds() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

[[nodiscard]] bool is_reliable(PacketReliability r) noexcept {
    return r == PacketReliability::ReliableOrdered || r == PacketReliability::ReliableUnordered;
}

} // namespace

// Address implementation moved to src/transport/address.cpp

// PacketHeader implementation moved to src/transport/packet.cpp

// ── UdpTransport::Impl ──────────────────────────────────────────────────────

struct UdpTransport::Impl {
    SocketType socket_ = INVALID_SOCKET_VAL;

    enum class Mode { None, Server, Client } mode_ = Mode::None;
    Address bound_address_;
    Address server_address_;

    struct Connection {
        Address address;
        enum class State { Connecting, Connected, Disconnecting } state = State::Connecting;
        double last_recv_time = 0.0;
        double last_send_time = 0.0;
        float smoothed_rtt = 0.1f;
        float rtt_variance = 0.05f;

        struct Channel {
            uint16_t local_seq = 0;
            uint16_t remote_seq = 0xFFFF;
            uint32_t ack_bits = 0;

            struct Pending {
                uint16_t sequence;
                double send_time;
                uint8_t retries;
                std::vector<uint8_t> data;
                bool fragmented;
                uint8_t priority = 0;
            };
            std::vector<Pending> pending;

            struct BufferedPacket {
                uint16_t sequence;
                uint8_t frag_index;
                uint8_t frag_count;
                std::vector<uint8_t> data;
            };
            std::vector<BufferedPacket> receive_buffer;

            // Phase 3: bandwidth limiting
            uint32_t bandwidth_limit = 0; // bytes/sec, 0 = unlimited
            uint32_t bytes_sent_this_second = 0;
        };
        std::array<Channel, 4> channels{};

        // Phase 3: global bandwidth
        uint32_t global_bandwidth_limit = 0;
        uint32_t bytes_sent_this_second = 0;
        double last_bandwidth_reset = 0.0;
    };
    std::vector<Connection> connections_;

    struct ReceivedPacket {
        Address sender;
        std::vector<uint8_t> data;
    };
    std::vector<ReceivedPacket> receive_queue_;
    std::size_t receive_read_idx_ = 0;

    struct FragmentAssembly {
        Address address;
        uint16_t base_sequence = 0;
        uint8_t frag_count = 0;
        uint8_t received_mask = 0;
        double start_time = 0.0;
        std::vector<std::vector<uint8_t>> fragments;
    };
    std::vector<FragmentAssembly> fragment_assemblies_;

    uint64_t packets_sent_ = 0;
    uint64_t packets_received_ = 0;
    uint64_t packets_acked_ = 0;

    [[nodiscard]] double now() const {
        return now_seconds();
    }

    bool create_socket();
    void close_socket();
    bool bind_socket(const Address& addr);
    bool send_raw(const Address& to, const PacketHeader& hdr, const uint8_t* payload, std::size_t len);

    Connection* find_connection(const Address& addr);
    Connection& get_or_create_connection(const Address& addr);

    bool send_to_connection(Connection& conn, const uint8_t* data, std::size_t length, PacketReliability reliability,
                            uint8_t priority = 0);
    void reset_bandwidth_counters(Connection& conn, double current_time);
    bool check_and_consume_bandwidth(Connection& conn, uint8_t ch, std::size_t packet_size);
    void process_packet(const Address& from, const PacketHeader& hdr, const uint8_t* payload, std::size_t len);
    void process_acks(Connection& conn, uint8_t ch, uint16_t ack, uint32_t ack_bits);
    void process_payload(Connection& conn, const PacketHeader& hdr, const uint8_t* payload, std::size_t len);
    void resend_pending(Connection& conn, double current_time);
    void send_handshake(const Address& to);
    void send_disconnect(const Address& to);
    void send_keepalive(Connection& conn);

    FragmentAssembly& find_or_create_fragment_assembly(const Address& addr, uint16_t sequence, uint8_t frag_count);
    void remove_fragment_assembly(const Address& addr, uint16_t sequence);
    void cleanup_fragments(double current_time);
    void cleanup_connections(double current_time);
    void push_receive(const Address& sender, std::vector<uint8_t> data);
};

// ── Socket helpers ──────────────────────────────────────────────────────────

bool UdpTransport::Impl::create_socket() {
    int fd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        int v6only = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
    } else {
        fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    if (fd < 0)
        return false;

#ifdef CAMPELLO_NET_PLATFORM_WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
    socket_ = fd;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    socket_ = fd;
#endif
    return true;
}

void UdpTransport::Impl::close_socket() {
    if (socket_ != INVALID_SOCKET_VAL) {
#ifdef CAMPELLO_NET_PLATFORM_WIN32
        closesocket(socket_);
#else
        ::close(socket_);
#endif
        socket_ = INVALID_SOCKET_VAL;
    }
}

bool UdpTransport::Impl::bind_socket(const Address& addr) {
    if (socket_ == INVALID_SOCKET_VAL)
        return false;
    auto* ss = reinterpret_cast<const sockaddr_storage*>(addr.raw_storage());
    return ::bind(socket_, reinterpret_cast<const sockaddr*>(ss), addr.raw_storage_size()) == 0;
}

bool UdpTransport::Impl::send_raw(const Address& to, const PacketHeader& hdr, const uint8_t* payload, std::size_t len) {
    if (socket_ == INVALID_SOCKET_VAL)
        return false;
    uint8_t buffer[MAX_PACKET_SIZE];
    if (PacketHeader::SIZE + len > MAX_PACKET_SIZE)
        return false;

    (void)hdr.serialize(buffer, PacketHeader::SIZE);
    if (len > 0 && payload != nullptr) {
        std::memcpy(buffer + PacketHeader::SIZE, payload, len);
    }

    auto* ss = reinterpret_cast<const sockaddr_storage*>(to.raw_storage());
    int sent = ::sendto(socket_, reinterpret_cast<const char*>(buffer), static_cast<int>(PacketHeader::SIZE + len), 0,
                        reinterpret_cast<const sockaddr*>(ss), to.raw_storage_size());

    if (sent == static_cast<int>(PacketHeader::SIZE + len)) {
        packets_sent_++;
        return true;
    }
    return false;
}

// ── Connection helpers ──────────────────────────────────────────────────────

UdpTransport::Impl::Connection* UdpTransport::Impl::find_connection(const Address& addr) {
    for (auto& c : connections_) {
        if (c.address == addr)
            return &c;
    }
    return nullptr;
}

UdpTransport::Impl::Connection& UdpTransport::Impl::get_or_create_connection(const Address& addr) {
    if (auto* c = find_connection(addr))
        return *c;
    Connection conn;
    conn.address = addr;
    conn.state = Connection::State::Connecting;
    conn.last_recv_time = now();
    connections_.push_back(std::move(conn));
    return connections_.back();
}

// ── Bandwidth helpers ───────────────────────────────────────────────────────

void UdpTransport::Impl::reset_bandwidth_counters(Connection& conn, double current_time) {
    if (current_time - conn.last_bandwidth_reset >= 1.0) {
        conn.bytes_sent_this_second = 0;
        conn.last_bandwidth_reset = current_time;
        for (auto& ch : conn.channels) {
            ch.bytes_sent_this_second = 0;
        }
    }
}

bool UdpTransport::Impl::check_and_consume_bandwidth(Connection& conn, uint8_t ch, std::size_t packet_size) {
    reset_bandwidth_counters(conn, now());

    if (conn.global_bandwidth_limit > 0) {
        if (conn.bytes_sent_this_second + packet_size > conn.global_bandwidth_limit) {
            return false;
        }
    }

    auto& channel = conn.channels[ch];
    if (channel.bandwidth_limit > 0) {
        if (channel.bytes_sent_this_second + packet_size > channel.bandwidth_limit) {
            return false;
        }
    }

    conn.bytes_sent_this_second += static_cast<uint32_t>(packet_size);
    channel.bytes_sent_this_second += static_cast<uint32_t>(packet_size);
    return true;
}

// ── Send helpers ────────────────────────────────────────────────────────────

bool UdpTransport::Impl::send_to_connection(Connection& conn, const uint8_t* data, std::size_t length,
                                            PacketReliability reliability, uint8_t priority) {
    if (conn.state != Connection::State::Connected && conn.state != Connection::State::Connecting)
        return false;

    uint8_t ch = static_cast<uint8_t>(reliability);
    auto& channel = conn.channels[ch];

    if (length <= MAX_PAYLOAD_SIZE) {
        std::size_t total_size = PacketHeader::SIZE + length;

        PacketHeader hdr;
        hdr.packet_type = static_cast<uint8_t>(PacketType::User);
        hdr.set_reliability(reliability);
        hdr.set_channel(ch);
        hdr.ack = channel.remote_seq;
        hdr.ack_bits = channel.ack_bits;
        hdr.payload_len = static_cast<uint16_t>(length);

        if (is_reliable(reliability)) {
            hdr.sequence = channel.local_seq++;
            channel.pending.push_back(
                {hdr.sequence, now(), 0, std::vector<uint8_t>(data, data + length), false, priority});
        } else {
            hdr.sequence = channel.local_seq++;
            if (!check_and_consume_bandwidth(conn, ch, total_size)) {
                return false; // Drop unreliable packet that exceeds bandwidth limit
            }
        }

        conn.last_send_time = now();
        return send_raw(conn.address, hdr, data, length);
    }

    // Fragmentation
    std::size_t fragment_size = MAX_PAYLOAD_SIZE;
    std::size_t num_fragments = (length + fragment_size - 1) / fragment_size;
    if (num_fragments > 255)
        return false;

    uint16_t base_sequence = channel.local_seq++;
    if (is_reliable(reliability)) {
        channel.pending.push_back({base_sequence, now(), 0, std::vector<uint8_t>(data, data + length), true, priority});
    }

    for (std::size_t i = 0; i < num_fragments; ++i) {
        std::size_t total_size = PacketHeader::SIZE + fragment_size;
        if (!is_reliable(reliability)) {
            if (!check_and_consume_bandwidth(conn, ch, total_size)) {
                return false; // Drop unreliable fragmented packet that exceeds bandwidth limit
            }
        }

        PacketHeader hdr;
        hdr.packet_type = static_cast<uint8_t>(PacketType::Fragment);
        hdr.set_reliability(reliability);
        hdr.set_channel(ch);
        hdr.sequence = base_sequence;
        hdr.ack = channel.remote_seq;
        hdr.ack_bits = channel.ack_bits;
        hdr.frag_index = static_cast<uint8_t>(i + 1);
        hdr.frag_count = static_cast<uint8_t>(num_fragments);

        std::size_t offset = i * fragment_size;
        std::size_t frag_len = std::min(fragment_size, length - offset);
        hdr.payload_len = static_cast<uint16_t>(frag_len);

        conn.last_send_time = now();
        if (!send_raw(conn.address, hdr, data + offset, frag_len))
            return false;
    }
    return true;
}

void UdpTransport::Impl::send_handshake(const Address& to) {
    PacketHeader hdr;
    hdr.packet_type = static_cast<uint8_t>(PacketType::Handshake);
    hdr.set_reliability(PacketReliability::Unreliable);
    hdr.set_channel(0);
    hdr.payload_len = 0;
    send_raw(to, hdr, nullptr, 0);
}

void UdpTransport::Impl::send_disconnect(const Address& to) {
    PacketHeader hdr;
    hdr.packet_type = static_cast<uint8_t>(PacketType::Disconnect);
    hdr.set_reliability(PacketReliability::Unreliable);
    hdr.set_channel(0);
    hdr.payload_len = 0;
    send_raw(to, hdr, nullptr, 0);
}

void UdpTransport::Impl::send_keepalive(Connection& conn) {
    PacketHeader hdr;
    hdr.packet_type = static_cast<uint8_t>(PacketType::Ack);
    hdr.set_reliability(PacketReliability::Unreliable);
    hdr.set_channel(0);
    auto& ch = conn.channels[0];
    hdr.ack = ch.remote_seq;
    hdr.ack_bits = ch.ack_bits;
    hdr.payload_len = 0;
    send_raw(conn.address, hdr, nullptr, 0);
}

// ── Ack & resend ────────────────────────────────────────────────────────────

void UdpTransport::Impl::process_acks(Connection& conn, uint8_t ch, uint16_t ack, uint32_t ack_bits) {
    if (ch >= conn.channels.size())
        return;
    auto& channel = conn.channels[ch];
    if (!is_reliable(static_cast<PacketReliability>(ch)))
        return;

    for (auto it = channel.pending.begin(); it != channel.pending.end();) {
        uint16_t seq = it->sequence;
        bool is_acked = false;

        if (seq == ack) {
            is_acked = true;
        } else {
            int32_t diff = static_cast<int32_t>(ack - seq);
            if (diff > 0 && diff <= 32) {
                is_acked = (ack_bits & (1u << (diff - 1))) != 0;
            }
        }

        if (is_acked) {
            float rtt_sample = static_cast<float>(now() - it->send_time);
            conn.smoothed_rtt = conn.smoothed_rtt * 0.875f + rtt_sample * 0.125f;
            conn.rtt_variance = conn.rtt_variance * 0.875f + std::abs(rtt_sample - conn.smoothed_rtt) * 0.125f;
            packets_acked_++;
            it = channel.pending.erase(it);
        } else {
            ++it;
        }
    }
}

void UdpTransport::Impl::resend_pending(Connection& conn, double current_time) {
    for (std::size_t ch = 0; ch < conn.channels.size(); ++ch) {
        auto& channel = conn.channels[ch];
        for (auto& pending : channel.pending) {
            float timeout = conn.smoothed_rtt + 4.0f * conn.rtt_variance;
            if (timeout < 0.05f)
                timeout = 0.05f;
            if (timeout > 1.0f)
                timeout = 1.0f;

            if (current_time - pending.send_time > timeout) {
                pending.send_time = current_time;
                pending.retries++;
                if (pending.retries > 10) {
                    conn.state = Connection::State::Disconnecting;
                    return;
                }

                auto reliability = static_cast<PacketReliability>(ch);
                if (pending.fragmented) {
                    std::size_t fragment_size = MAX_PAYLOAD_SIZE;
                    std::size_t num_fragments = (pending.data.size() + fragment_size - 1) / fragment_size;
                    for (std::size_t i = 0; i < num_fragments; ++i) {
                        std::size_t total_size = PacketHeader::SIZE + fragment_size;
                        if (!check_and_consume_bandwidth(conn, static_cast<uint8_t>(ch), total_size)) {
                            continue; // Skip this fragment if bandwidth exceeded, will retry next tick
                        }

                        PacketHeader hdr;
                        hdr.packet_type = static_cast<uint8_t>(PacketType::Fragment);
                        hdr.set_reliability(reliability);
                        hdr.set_channel(static_cast<uint8_t>(ch));
                        hdr.sequence = pending.sequence;
                        hdr.ack = channel.remote_seq;
                        hdr.ack_bits = channel.ack_bits;
                        hdr.frag_index = static_cast<uint8_t>(i + 1);
                        hdr.frag_count = static_cast<uint8_t>(num_fragments);

                        std::size_t offset = i * fragment_size;
                        std::size_t frag_len = std::min(fragment_size, pending.data.size() - offset);
                        hdr.payload_len = static_cast<uint16_t>(frag_len);
                        send_raw(conn.address, hdr, pending.data.data() + offset, frag_len);
                    }
                } else {
                    std::size_t total_size = PacketHeader::SIZE + pending.data.size();
                    if (!check_and_consume_bandwidth(conn, static_cast<uint8_t>(ch), total_size)) {
                        continue; // Skip this resend if bandwidth exceeded, will retry next tick
                    }

                    PacketHeader hdr;
                    hdr.packet_type = static_cast<uint8_t>(PacketType::User);
                    hdr.set_reliability(reliability);
                    hdr.set_channel(static_cast<uint8_t>(ch));
                    hdr.sequence = pending.sequence;
                    hdr.ack = channel.remote_seq;
                    hdr.ack_bits = channel.ack_bits;
                    hdr.payload_len = static_cast<uint16_t>(pending.data.size());
                    send_raw(conn.address, hdr, pending.data.data(), pending.data.size());
                }
            }
        }
    }
}

// ── Fragment helpers ────────────────────────────────────────────────────────

UdpTransport::Impl::FragmentAssembly&
UdpTransport::Impl::find_or_create_fragment_assembly(const Address& addr, uint16_t sequence, uint8_t frag_count) {
    for (auto& fa : fragment_assemblies_) {
        if (fa.address == addr && fa.base_sequence == sequence)
            return fa;
    }
    FragmentAssembly fa;
    fa.address = addr;
    fa.base_sequence = sequence;
    fa.frag_count = frag_count;
    fa.received_mask = 0;
    fa.start_time = now();
    fa.fragments.resize(frag_count);
    fragment_assemblies_.push_back(std::move(fa));
    return fragment_assemblies_.back();
}

void UdpTransport::Impl::remove_fragment_assembly(const Address& addr, uint16_t sequence) {
    fragment_assemblies_.erase(std::remove_if(fragment_assemblies_.begin(), fragment_assemblies_.end(),
                                              [&](const FragmentAssembly& fa) {
                                                  return fa.address == addr && fa.base_sequence == sequence;
                                              }),
                               fragment_assemblies_.end());
}

void UdpTransport::Impl::cleanup_fragments(double current_time) {
    fragment_assemblies_.erase(std::remove_if(fragment_assemblies_.begin(), fragment_assemblies_.end(),
                                              [&](const FragmentAssembly& fa) {
                                                  return current_time - fa.start_time > 2.0;
                                              }),
                               fragment_assemblies_.end());
}

// ── Receive helpers ─────────────────────────────────────────────────────────

void UdpTransport::Impl::push_receive(const Address& sender, std::vector<uint8_t> data) {
    receive_queue_.push_back({sender, std::move(data)});
}

void UdpTransport::Impl::process_payload(Connection& conn, const PacketHeader& hdr, const uint8_t* payload,
                                         std::size_t len) {
    if (hdr.frag_count > 1) {
        auto& fa = find_or_create_fragment_assembly(conn.address, hdr.sequence, hdr.frag_count);
        if (fa.fragments[hdr.frag_index - 1].empty()) {
            fa.fragments[hdr.frag_index - 1].assign(payload, payload + len);
            fa.received_mask |= (1u << (hdr.frag_index - 1));

            if (fa.received_mask == ((1u << hdr.frag_count) - 1u)) {
                std::size_t total = 0;
                for (auto& f : fa.fragments)
                    total += f.size();
                std::vector<uint8_t> assembled;
                assembled.reserve(total);
                for (auto& f : fa.fragments)
                    assembled.insert(assembled.end(), f.begin(), f.end());
                push_receive(conn.address, std::move(assembled));
                remove_fragment_assembly(conn.address, hdr.sequence);
            }
        }
    } else {
        push_receive(conn.address, std::vector<uint8_t>(payload, payload + len));
    }
}

void UdpTransport::Impl::process_packet(const Address& from, const PacketHeader& hdr, const uint8_t* payload,
                                        std::size_t len) {
    if (hdr.protocol_id != PacketHeader::PROTOCOL_ID)
        return;

    auto& conn = get_or_create_connection(from);
    conn.last_recv_time = now();

    uint8_t ch = hdr.channel();
    if (ch >= conn.channels.size())
        return;
    auto& channel = conn.channels[ch];

    process_acks(conn, ch, hdr.ack, hdr.ack_bits);

    auto pkt_type = static_cast<PacketType>(hdr.packet_type);
    if (pkt_type == PacketType::Handshake) {
        if (conn.state == Connection::State::Connecting) {
            conn.state = Connection::State::Connected;
        }
        send_handshake(from);
        return;
    }
    if (pkt_type == PacketType::Disconnect) {
        conn.state = Connection::State::Disconnecting;
        return;
    }
    if (pkt_type == PacketType::Ack) {
        return;
    }

    auto reliability = hdr.reliability();
    uint16_t seq = hdr.sequence;

    if (reliability == PacketReliability::ReliableOrdered) {
        uint16_t expected = channel.remote_seq + 1;
        if (seq == expected) {
            channel.remote_seq = seq;
            channel.ack_bits = (channel.ack_bits << 1) | 1;
            process_payload(conn, hdr, payload, len);
        } else if (seq > expected) {
            // Buffer out-of-order packets (non-fragmented only for Phase 1)
            if (hdr.frag_count <= 1) {
                bool exists = false;
                for (auto& p : channel.receive_buffer) {
                    if (p.sequence == seq) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    channel.receive_buffer.push_back({seq, 0, 1, std::vector<uint8_t>(payload, payload + len)});
                }
            }
        } else {
            return; // old packet
        }
    } else if (reliability == PacketReliability::ReliableUnordered) {
        channel.remote_seq = seq;
        channel.ack_bits = (channel.ack_bits << 1) | 1;
        process_payload(conn, hdr, payload, len);
    } else if (reliability == PacketReliability::UnreliableSequenced) {
        if (seq <= channel.remote_seq)
            return;
        channel.remote_seq = seq;
        process_payload(conn, hdr, payload, len);
    } else {
        process_payload(conn, hdr, payload, len);
    }
}

void UdpTransport::Impl::cleanup_connections(double current_time) {
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                      [&](const Connection& c) {
                                          return c.state == Connection::State::Disconnecting ||
                                                 (current_time - c.last_recv_time > 5.0);
                                      }),
                       connections_.end());
}

// ── UdpTransport public API ─────────────────────────────────────────────────

UdpTransport::UdpTransport() : impl_(std::make_unique<Impl>()) {}

UdpTransport::~UdpTransport() {
    disconnect();
}

bool UdpTransport::bind(const Address& address) {
    disconnect();
    if (!impl_->create_socket())
        return false;
    if (!impl_->bind_socket(address)) {
        impl_->close_socket();
        return false;
    }
    impl_->mode_ = Impl::Mode::Server;
    impl_->bound_address_ = address;
    return true;
}

bool UdpTransport::connect(const Address& address) {
    disconnect();
    if (!impl_->create_socket())
        return false;

    Address any(0);
    if (!impl_->bind_socket(any)) {
        impl_->close_socket();
        return false;
    }

    impl_->mode_ = Impl::Mode::Client;
    impl_->server_address_ = address;

    Impl::Connection conn;
    conn.address = address;
    conn.state = Impl::Connection::State::Connecting;
    conn.last_recv_time = impl_->now();
    impl_->connections_.push_back(std::move(conn));

    impl_->send_handshake(address);
    return true;
}

void UdpTransport::disconnect() {
    if (!impl_)
        return;
    for (auto& conn : impl_->connections_) {
        if (conn.state == Impl::Connection::State::Connected) {
            impl_->send_disconnect(conn.address);
        }
    }
    impl_->close_socket();
    impl_->connections_.clear();
    impl_->receive_queue_.clear();
    impl_->receive_read_idx_ = 0;
    impl_->fragment_assemblies_.clear();
    impl_->mode_ = Impl::Mode::None;
    impl_->packets_sent_ = 0;
    impl_->packets_received_ = 0;
    impl_->packets_acked_ = 0;
}

bool UdpTransport::is_connected() const noexcept {
    if (!impl_)
        return false;
    if (impl_->mode_ == Impl::Mode::Client) {
        return !impl_->connections_.empty() && impl_->connections_[0].state == Impl::Connection::State::Connected;
    }
    return impl_->mode_ == Impl::Mode::Server && impl_->socket_ != INVALID_SOCKET_VAL;
}

bool UdpTransport::send(const uint8_t* data, std::size_t length, PacketReliability reliability) {
    if (!impl_)
        return false;
    if (impl_->mode_ == Impl::Mode::Client) {
        if (impl_->connections_.empty())
            return false;
        return impl_->send_to_connection(impl_->connections_[0], data, length, reliability);
    }
    if (impl_->mode_ == Impl::Mode::Server) {
        bool any = false;
        for (auto& conn : impl_->connections_) {
            if (impl_->send_to_connection(conn, data, length, reliability))
                any = true;
        }
        return any;
    }
    return false;
}

bool UdpTransport::send_to(const Address& address, const uint8_t* data, std::size_t length,
                           PacketReliability reliability) {
    if (!impl_)
        return false;
    if (auto* conn = impl_->find_connection(address)) {
        return impl_->send_to_connection(*conn, data, length, reliability);
    }
    return false;
}

void UdpTransport::poll() {
    if (!impl_ || impl_->socket_ == INVALID_SOCKET_VAL)
        return;

    double t = impl_->now();

    while (true) {
        uint8_t buffer[MAX_PACKET_SIZE];
        sockaddr_storage from_ss{};
        socklen_t from_len = sizeof(from_ss);

        int received = ::recvfrom(impl_->socket_, reinterpret_cast<char*>(buffer), MAX_PACKET_SIZE, 0,
                                  reinterpret_cast<sockaddr*>(&from_ss), &from_len);
        if (received < 0) {
#ifdef CAMPELLO_NET_PLATFORM_WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK)
                break;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
#endif
            break;
        }
        if (received < static_cast<int>(PacketHeader::SIZE))
            continue;

        PacketHeader hdr;
        if (!hdr.deserialize(buffer, PacketHeader::SIZE))
            continue;

        Address from_addr;
        from_addr.set_raw_storage(reinterpret_cast<const std::byte*>(&from_ss), static_cast<uint8_t>(from_len));

        impl_->process_packet(from_addr, hdr, buffer + PacketHeader::SIZE,
                              static_cast<std::size_t>(received) - PacketHeader::SIZE);
        impl_->packets_received_++;
    }

    for (auto& conn : impl_->connections_) {
        if (conn.state == Impl::Connection::State::Connected) {
            impl_->resend_pending(conn, t);
            if (t - conn.last_send_time > 0.1) {
                impl_->send_keepalive(conn);
                conn.last_send_time = t;
            }
        }
    }

    impl_->cleanup_fragments(t);
    impl_->cleanup_connections(t);
}

bool UdpTransport::pop_receive(uint8_t* buffer, std::size_t max_length, std::size_t& out_length, Address& out_sender) {
    if (!impl_)
        return false;
    if (impl_->receive_read_idx_ >= impl_->receive_queue_.size()) {
        impl_->receive_queue_.clear();
        impl_->receive_read_idx_ = 0;
        return false;
    }

    auto& pkt = impl_->receive_queue_[impl_->receive_read_idx_++];
    out_sender = pkt.sender;
    out_length = std::min(max_length, pkt.data.size());
    std::memcpy(buffer, pkt.data.data(), out_length);
    return true;
}

float UdpTransport::rtt() const noexcept {
    if (!impl_ || impl_->connections_.empty())
        return 0.0f;
    float sum = 0.0f;
    for (auto& c : impl_->connections_)
        sum += c.smoothed_rtt;
    return sum / static_cast<float>(impl_->connections_.size());
}

float UdpTransport::packet_loss() const noexcept {
    if (!impl_ || impl_->packets_sent_ == 0)
        return 0.0f;
    uint64_t unacked = impl_->packets_sent_ > impl_->packets_acked_ ? impl_->packets_sent_ - impl_->packets_acked_ : 0;
    return static_cast<float>(unacked) / static_cast<float>(impl_->packets_sent_);
}

bool UdpTransport::send_with_priority(const Address& address, const uint8_t* data, std::size_t length,
                                      PacketReliability reliability, uint8_t priority) {
    if (!impl_)
        return false;
    if (auto* conn = impl_->find_connection(address)) {
        return impl_->send_to_connection(*conn, data, length, reliability, priority);
    }
    return false;
}

void UdpTransport::set_connection_bandwidth_limit(const Address& address, std::uint32_t bytes_per_second) {
    if (!impl_)
        return;
    if (auto* conn = impl_->find_connection(address)) {
        conn->global_bandwidth_limit = bytes_per_second;
    }
}

void UdpTransport::set_channel_bandwidth_limit(const Address& address, PacketReliability reliability,
                                               std::uint32_t bytes_per_second) {
    if (!impl_)
        return;
    if (auto* conn = impl_->find_connection(address)) {
        uint8_t ch = static_cast<uint8_t>(reliability);
        if (ch < conn->channels.size()) {
            conn->channels[ch].bandwidth_limit = bytes_per_second;
        }
    }
}

float UdpTransport::get_connection_rtt(const Address& address) const noexcept {
    if (!impl_)
        return 0.0f;
    if (auto* conn = impl_->find_connection(address)) {
        return conn->smoothed_rtt;
    }
    return 0.0f;
}

float UdpTransport::get_connection_packet_loss(const Address& address) const noexcept {
    if (!impl_)
        return 0.0f;
    if (auto* conn = impl_->find_connection(address)) {
        // Per-connection packet loss is approximate: ratio of pending packets to total sent
        uint64_t total_pending = 0;
        for (auto& ch : conn->channels) {
            total_pending += ch.pending.size();
        }
        // Heuristic: if we have no send history, return 0
        if (total_pending == 0)
            return 0.0f;
        // This is a rough estimate based on pending unacked packets
        // A more accurate measure would require per-connection sent/acked counters
        return static_cast<float>(total_pending) / (static_cast<float>(total_pending) + 100.0f);
    }
    return 0.0f;
}

} // namespace systems::leal::campello_net::transport

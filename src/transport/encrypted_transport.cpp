#include "campello_net/transport/encrypted_transport.hpp"

#include "campello_net/crypto/chachapoly.hpp"

#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace systems::leal::campello_net::transport {

namespace {

using crypto::ChaCha20Poly1305;

static constexpr std::size_t NONCE_SIZE = ChaCha20Poly1305::NONCE_SIZE;
static constexpr std::size_t TAG_SIZE = ChaCha20Poly1305::TAG_SIZE;
static constexpr std::size_t OVERHEAD = NONCE_SIZE + TAG_SIZE; // 28 bytes

// Replay window: accept counters within 64 of the highest seen.
static constexpr std::uint64_t REPLAY_WINDOW = 64;

// Direction byte in nonce[0]
static constexpr std::uint8_t DIR_CLIENT_TO_SERVER = 0x00;
static constexpr std::uint8_t DIR_SERVER_TO_CLIENT = 0x01;

struct CounterState {
    std::uint64_t outbound = 0;
    std::uint64_t highest_inbound = 0;
    std::uint64_t seen_bitmap = 0; // bit i set => counter (highest_inbound - i) already received
};

inline void store_counter_le(std::uint8_t* dst, std::uint64_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v);
    dst[1] = static_cast<std::uint8_t>(v >> 8);
    dst[2] = static_cast<std::uint8_t>(v >> 16);
    dst[3] = static_cast<std::uint8_t>(v >> 24);
    dst[4] = static_cast<std::uint8_t>(v >> 32);
    dst[5] = static_cast<std::uint8_t>(v >> 40);
    dst[6] = static_cast<std::uint8_t>(v >> 48);
    dst[7] = static_cast<std::uint8_t>(v >> 56);
}

inline std::uint64_t load_counter_le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint64_t>(src[0]) | (static_cast<std::uint64_t>(src[1]) << 8) |
           (static_cast<std::uint64_t>(src[2]) << 16) | (static_cast<std::uint64_t>(src[3]) << 24) |
           (static_cast<std::uint64_t>(src[4]) << 32) | (static_cast<std::uint64_t>(src[5]) << 40) |
           (static_cast<std::uint64_t>(src[6]) << 48) | (static_cast<std::uint64_t>(src[7]) << 56);
}

} // unnamed namespace

struct EncryptedTransport::Impl {
    std::unique_ptr<ITransport> inner;
    std::array<std::uint8_t, KEY_SIZE> client_to_server_key{};
    std::array<std::uint8_t, KEY_SIZE> server_to_client_key{};

    // Per-address counter state for server mode
    std::unordered_map<std::size_t, CounterState> per_address;

    // Client mode has a single peer (the server)
    CounterState client_state;

    // Scratch buffers (avoid malloc on hot path after first growth)
    std::vector<std::uint8_t> recv_scratch;
    std::vector<std::uint8_t> send_scratch;

    std::size_t address_hash(const Address& addr) const noexcept {
        std::size_t h = 0;
        std::size_t sz = addr.raw_storage_size();
        const std::byte* data = addr.raw_storage();
        for (std::size_t i = 0; i < sz; ++i) {
            h = h * 31 + static_cast<std::size_t>(static_cast<std::uint8_t>(data[i]));
        }
        return h;
    }

    CounterState& get_counter(const Address* addr) {
        if (addr) {
            return per_address[address_hash(*addr)];
        }
        return client_state;
    }
};

EncryptedTransport::EncryptedTransport(std::unique_ptr<ITransport> inner, std::span<const std::uint8_t> key)
    : impl_(std::make_unique<Impl>()) {
    impl_->inner = std::move(inner);
    std::array<std::uint8_t, KEY_SIZE> master_key{};
    std::size_t to_copy = key.size() < KEY_SIZE ? key.size() : KEY_SIZE;
    std::memcpy(master_key.data(), key.data(), to_copy);
    ChaCha20Poly1305::derive_keys(master_key.data(), impl_->client_to_server_key.data(),
                                  impl_->server_to_client_key.data());
}

EncryptedTransport::~EncryptedTransport() = default;

EncryptedTransport::EncryptedTransport(EncryptedTransport&&) noexcept = default;
EncryptedTransport& EncryptedTransport::operator=(EncryptedTransport&&) noexcept = default;

bool EncryptedTransport::bind(const Address& address) {
    return impl_->inner->bind(address);
}

bool EncryptedTransport::connect(const Address& address) {
    return impl_->inner->connect(address);
}

void EncryptedTransport::disconnect() {
    impl_->inner->disconnect();
}

bool EncryptedTransport::is_connected() const noexcept {
    return impl_->inner->is_connected();
}

bool EncryptedTransport::send(const std::uint8_t* data, std::size_t length, PacketReliability reliability) {
    // Client -> Server
    auto& st = impl_->client_state;
    std::uint64_t counter = st.outbound++;

    std::uint8_t nonce[NONCE_SIZE] = {};
    nonce[0] = DIR_CLIENT_TO_SERVER;
    store_counter_le(nonce + 4, counter);

    std::size_t sealed_len = length + OVERHEAD;
    if (impl_->send_scratch.size() < sealed_len)
        impl_->send_scratch.resize(sealed_len);

    std::uint8_t* scratch = impl_->send_scratch.data();

    if (!ChaCha20Poly1305::seal(impl_->client_to_server_key.data(), nonce, nullptr, 0, data, length,
                                scratch + NONCE_SIZE, length + TAG_SIZE)) {
        return false;
    }

    std::memcpy(scratch, nonce, NONCE_SIZE);
    return impl_->inner->send(scratch, sealed_len, reliability);
}

bool EncryptedTransport::send_to(const Address& address, const std::uint8_t* data, std::size_t length,
                                 PacketReliability reliability) {
    // Server -> Client
    auto& st = impl_->get_counter(&address);
    std::uint64_t counter = st.outbound++;

    std::uint8_t nonce[NONCE_SIZE] = {};
    nonce[0] = DIR_SERVER_TO_CLIENT;
    store_counter_le(nonce + 4, counter);

    std::size_t sealed_len = length + OVERHEAD;
    if (impl_->send_scratch.size() < sealed_len)
        impl_->send_scratch.resize(sealed_len);

    std::uint8_t* scratch = impl_->send_scratch.data();

    if (!ChaCha20Poly1305::seal(impl_->server_to_client_key.data(), nonce, nullptr, 0, data, length,
                                scratch + NONCE_SIZE, length + TAG_SIZE)) {
        return false;
    }

    std::memcpy(scratch, nonce, NONCE_SIZE);
    return impl_->inner->send_to(address, scratch, sealed_len, reliability);
}

void EncryptedTransport::poll() {
    impl_->inner->poll();
}

bool EncryptedTransport::pop_receive(std::uint8_t* buffer, std::size_t max_length, std::size_t& out_length,
                                     Address& out_sender) {
    if (impl_->recv_scratch.size() < max_length + OVERHEAD)
        impl_->recv_scratch.resize(max_length + OVERHEAD);

    std::size_t inner_len = 0;
    if (!impl_->inner->pop_receive(impl_->recv_scratch.data(), impl_->recv_scratch.size(), inner_len, out_sender)) {
        return false;
    }

    if (inner_len < OVERHEAD) {
        // Too short — drop.
        return false;
    }

    const std::uint8_t* received = impl_->recv_scratch.data();
    std::uint8_t nonce[NONCE_SIZE];
    std::memcpy(nonce, received, NONCE_SIZE);

    std::size_t ciphertext_len = inner_len - NONCE_SIZE;
    std::size_t plaintext_len = ciphertext_len - TAG_SIZE;
    if (plaintext_len > max_length)
        return false;

    // Select key and counter state based on direction
    const std::uint8_t* key = nullptr;
    CounterState* st = nullptr;
    if (nonce[0] == DIR_CLIENT_TO_SERVER) {
        // We must be the server receiving from a client
        key = impl_->client_to_server_key.data();
        st = &impl_->per_address[impl_->address_hash(out_sender)];
    } else if (nonce[0] == DIR_SERVER_TO_CLIENT) {
        // We must be the client receiving from the server
        key = impl_->server_to_client_key.data();
        st = &impl_->client_state;
    } else {
        return false; // unknown direction
    }

    // Replay check
    std::uint64_t counter = load_counter_le(nonce + 4);
    if (counter + REPLAY_WINDOW <= st->highest_inbound) {
        return false; // too old
    }

    if (counter > st->highest_inbound) {
        std::uint64_t shift = counter - st->highest_inbound;
        if (shift >= 64) {
            st->seen_bitmap = 0;
        } else {
            st->seen_bitmap <<= shift;
        }
        st->highest_inbound = counter;
    }

    std::uint64_t diff = st->highest_inbound - counter;
    if (diff < 64) {
        std::uint64_t bit = 1ULL << diff;
        if (st->seen_bitmap & bit) {
            return false; // exact duplicate
        }
        st->seen_bitmap |= bit;
    }

    if (!ChaCha20Poly1305::open(key, nonce, nullptr, 0, received + NONCE_SIZE, ciphertext_len, buffer, max_length)) {
        return false;
    }

    out_length = plaintext_len;
    return true;
}

float EncryptedTransport::rtt() const noexcept {
    return impl_->inner->rtt();
}

float EncryptedTransport::packet_loss() const noexcept {
    return impl_->inner->packet_loss();
}

float EncryptedTransport::get_connection_rtt(const Address& address) const noexcept {
    return impl_->inner->get_connection_rtt(address);
}

float EncryptedTransport::get_connection_packet_loss(const Address& address) const noexcept {
    return impl_->inner->get_connection_packet_loss(address);
}

} // namespace systems::leal::campello_net::transport

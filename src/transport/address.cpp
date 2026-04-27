#include "campello_net/transport/address.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#ifndef CAMPELLO_NET_PLATFORM_WASM
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
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#endif

namespace systems::leal::campello_net::transport {

// ── Platform-specific Address implementation ────────────────────────────────

#ifndef CAMPELLO_NET_PLATFORM_WASM

// Native implementation: sockaddr_storage backed (IPv4 / IPv6)

Address::Address(uint16_t port) noexcept {
    std::string port_str = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    if (getaddrinfo(nullptr, port_str.c_str(), &hints, &result) == 0 && result) {
        std::memcpy(storage_.data(), result->ai_addr, result->ai_addrlen);
        storage_len_ = static_cast<uint8_t>(result->ai_addrlen);
        valid_ = true;
        freeaddrinfo(result);
    } else {
        hints.ai_family = AF_INET;
        if (getaddrinfo(nullptr, port_str.c_str(), &hints, &result) == 0 && result) {
            std::memcpy(storage_.data(), result->ai_addr, result->ai_addrlen);
            storage_len_ = static_cast<uint8_t>(result->ai_addrlen);
            valid_ = true;
            freeaddrinfo(result);
        }
    }
}

Address::Address(const std::string& ip, uint16_t port) {
    // Always store as IPv6 (IPv4-mapped for v4 inputs) so dual-stack sockets work uniformly.
    if (ip.find(':') == std::string::npos) {
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);
        sin6.sin6_addr.s6_addr[10] = 0xFF;
        sin6.sin6_addr.s6_addr[11] = 0xFF;
        if (inet_pton(AF_INET, ip.c_str(), &sin6.sin6_addr.s6_addr[12]) == 1) {
            std::memcpy(storage_.data(), &sin6, sizeof(sin6));
            storage_len_ = sizeof(sin6);
            valid_ = true;
        }
    } else {
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);
        if (inet_pton(AF_INET6, ip.c_str(), &sin6.sin6_addr) == 1) {
            std::memcpy(storage_.data(), &sin6, sizeof(sin6));
            storage_len_ = sizeof(sin6);
            valid_ = true;
        }
    }
}

uint16_t Address::port() const noexcept {
    if (!valid_)
        return 0;
    auto* ss = reinterpret_cast<const sockaddr_storage*>(storage_.data());
    if (ss->ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(ss)->sin6_port);
    }
    if (ss->ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(ss)->sin_port);
    }
    return 0;
}

std::string Address::ip() const {
    if (!valid_)
        return "";
    auto* ss = reinterpret_cast<const sockaddr_storage*>(storage_.data());
    if (ss->ss_family == AF_INET6) {
        auto* sin6 = reinterpret_cast<const sockaddr_in6*>(ss);
        const uint8_t* b = sin6->sin6_addr.s6_addr;
        // Detect IPv4-mapped IPv6 and return clean IPv4 string.
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0 && b[6] == 0 && b[7] == 0 &&
            b[8] == 0 && b[9] == 0 && b[10] == 0xFF && b[11] == 0xFF) {
            char buffer[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, b + 12, buffer, sizeof(buffer));
            return buffer;
        }
        char buffer[INET6_ADDRSTRLEN] = {};
        inet_ntop(AF_INET6, &sin6->sin6_addr, buffer, sizeof(buffer));
        return buffer;
    }
    return "";
}

std::string Address::to_string() const {
    return ip() + ":" + std::to_string(port());
}

bool Address::operator==(const Address& other) const noexcept {
    if (valid_ != other.valid_)
        return false;
    if (!valid_)
        return true;

    auto* sa = reinterpret_cast<const sockaddr_storage*>(storage_.data());
    auto* sb = reinterpret_cast<const sockaddr_storage*>(other.storage_.data());
    if (sa->ss_family != sb->ss_family)
        return false;

    if (sa->ss_family == AF_INET6) {
        auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
        auto* b6 = reinterpret_cast<const sockaddr_in6*>(sb);
        return a6->sin6_port == b6->sin6_port &&
               std::memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(a6->sin6_addr)) == 0;
    }
    if (sa->ss_family == AF_INET) {
        auto* a4 = reinterpret_cast<const sockaddr_in*>(sa);
        auto* b4 = reinterpret_cast<const sockaddr_in*>(sb);
        return a4->sin_port == b4->sin_port && a4->sin_addr.s_addr == b4->sin_addr.s_addr;
    }
    return false;
}

bool Address::operator!=(const Address& other) const noexcept {
    return !(*this == other);
}

bool Address::operator<(const Address& other) const noexcept {
    if (valid_ != other.valid_)
        return !valid_;

    auto* sa = reinterpret_cast<const sockaddr_storage*>(storage_.data());
    auto* sb = reinterpret_cast<const sockaddr_storage*>(other.storage_.data());
    if (sa->ss_family != sb->ss_family)
        return sa->ss_family < sb->ss_family;

    if (sa->ss_family == AF_INET6) {
        auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
        auto* b6 = reinterpret_cast<const sockaddr_in6*>(sb);
        if (a6->sin6_port != b6->sin6_port)
            return ntohs(a6->sin6_port) < ntohs(b6->sin6_port);
        return std::memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(a6->sin6_addr)) < 0;
    }
    if (sa->ss_family == AF_INET) {
        auto* a4 = reinterpret_cast<const sockaddr_in*>(sa);
        auto* b4 = reinterpret_cast<const sockaddr_in*>(sb);
        if (a4->sin_port != b4->sin_port)
            return ntohs(a4->sin_port) < ntohs(b4->sin_port);
        return a4->sin_addr.s_addr < b4->sin_addr.s_addr;
    }
    return false;
}

void Address::set_raw_storage(const std::byte* data, uint8_t len) noexcept {
    if (len <= storage_.size()) {
        std::memcpy(storage_.data(), data, len);
        auto* ss = reinterpret_cast<sockaddr_storage*>(storage_.data());
        if (ss->ss_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<sockaddr_in6*>(ss);
            sin6->sin6_flowinfo = 0;
            sin6->sin6_scope_id = 0;
        }
        storage_len_ = len;
        valid_ = true;
    }
}

#else // CAMPELLO_NET_PLATFORM_WASM

// WASM implementation: simple host string + port stored in the 128-byte buffer

namespace {

struct WasmAddrData {
    uint16_t port;
    uint8_t host_len;
    char host[125];
};
static_assert(sizeof(WasmAddrData) <= 128);

WasmAddrData* wasm_data(std::array<std::byte, 128>& storage) {
    return reinterpret_cast<WasmAddrData*>(storage.data());
}

const WasmAddrData* wasm_data(const std::array<std::byte, 128>& storage) {
    return reinterpret_cast<const WasmAddrData*>(storage.data());
}

} // namespace

Address::Address(uint16_t port) noexcept {
    auto* d = wasm_data(storage_);
    d->port = port;
    std::strncpy(d->host, "0.0.0.0", sizeof(d->host));
    d->host_len = static_cast<uint8_t>(std::strlen(d->host));
    storage_len_ = sizeof(WasmAddrData);
    valid_ = true;
}

Address::Address(const std::string& ip, uint16_t port) {
    auto* d = wasm_data(storage_);
    d->port = port;
    std::size_t copy_len = std::min(ip.size(), sizeof(d->host) - 1);
    std::memcpy(d->host, ip.data(), copy_len);
    d->host[copy_len] = '\0';
    d->host_len = static_cast<uint8_t>(copy_len);
    storage_len_ = sizeof(WasmAddrData);
    valid_ = true;
}

uint16_t Address::port() const noexcept {
    if (!valid_)
        return 0;
    return wasm_data(storage_)->port;
}

std::string Address::ip() const {
    if (!valid_)
        return "";
    const auto* d = wasm_data(storage_);
    return std::string(d->host, d->host_len);
}

std::string Address::to_string() const {
    return ip() + ":" + std::to_string(port());
}

bool Address::operator==(const Address& other) const noexcept {
    if (valid_ != other.valid_)
        return false;
    if (!valid_)
        return true;
    const auto* a = wasm_data(storage_);
    const auto* b = wasm_data(other.storage_);
    return a->port == b->port && a->host_len == b->host_len &&
           std::memcmp(a->host, b->host, a->host_len) == 0;
}

bool Address::operator!=(const Address& other) const noexcept {
    return !(*this == other);
}

bool Address::operator<(const Address& other) const noexcept {
    if (valid_ != other.valid_)
        return !valid_;
    const auto* a = wasm_data(storage_);
    const auto* b = wasm_data(other.storage_);
    if (a->port != b->port)
        return a->port < b->port;
    int cmp = std::memcmp(a->host, b->host, std::min(a->host_len, b->host_len));
    if (cmp != 0)
        return cmp < 0;
    return a->host_len < b->host_len;
}

void Address::set_raw_storage(const std::byte* data, uint8_t len) noexcept {
    if (len <= storage_.size()) {
        std::memcpy(storage_.data(), data, len);
        storage_len_ = len;
        valid_ = true;
    }
}

#endif // CAMPELLO_NET_PLATFORM_WASM

} // namespace systems::leal::campello_net::transport

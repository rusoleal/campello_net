#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace systems::leal::campello_net::serialization {

/// A bidirectional bit-level stream for compact binary serialization.
///
/// Bits are packed MSB-first within each byte. Multi-byte values written at
/// byte boundaries are stored in network (big-endian) byte order.
class BitStream {
public:
    BitStream() = default;
    explicit BitStream(std::span<const uint8_t> data)
        : buffer_(data.begin(), data.end()), write_bit_pos_(buffer_.size() * 8) {}

    // ── Writing ─────────────────────────────────────────────────────────────

    /// Write the lowest @p num_bits of @p value (MSB-first).
    void write_bits(uint64_t value, uint8_t num_bits);

    void write_bool(bool value) {
        write_bits(value ? 1u : 0u, 1);
    }

    void write_uint8(uint8_t value) {
        write_bits(value, 8);
    }
    void write_uint16(uint16_t value);
    void write_uint32(uint32_t value);
    void write_uint64(uint64_t value);

    void write_int8(int8_t value) {
        write_bits(static_cast<uint64_t>(static_cast<uint8_t>(value)), 8);
    }
    void write_int16(int16_t value) {
        write_uint16(static_cast<uint16_t>(value));
    }
    void write_int32(int32_t value) {
        write_uint32(static_cast<uint32_t>(value));
    }
    void write_int64(int64_t value) {
        write_uint64(static_cast<uint64_t>(value));
    }

    /// Protobuf-style varint (7 bits per byte, continuation high bit).
    void write_varint(uint64_t value);

    /// Full-precision float.
    void write_float(float value);
    /// Full-precision double.
    void write_double(double value);

    /// IEEE 754 half-precision (16-bit) float.
    void write_half(float value);

    /// Raw byte copy (must be byte-aligned).
    void write_bytes(const uint8_t* data, std::size_t len);

    void write_string(const std::string& str);

    template <typename T> void write_array(std::span<const T> values);

    // ── Reading ─────────────────────────────────────────────────────────────

    /// Read @p num_bits into @p value (MSB-first). Returns false on overflow.
    bool read_bits(uint64_t& value, uint8_t num_bits);

    bool read_bool(bool& value) {
        uint64_t v = 0;
        if (!read_bits(v, 1))
            return false;
        value = v != 0;
        return true;
    }

    bool read_uint8(uint8_t& value) {
        uint64_t v = 0;
        if (!read_bits(v, 8))
            return false;
        value = static_cast<uint8_t>(v);
        return true;
    }
    bool read_uint16(uint16_t& value);
    bool read_uint32(uint32_t& value);
    bool read_uint64(uint64_t& value);

    bool read_int8(int8_t& value) {
        uint8_t v = 0;
        if (!read_uint8(v))
            return false;
        value = static_cast<int8_t>(v);
        return true;
    }
    bool read_int16(int16_t& value) {
        uint16_t v = 0;
        if (!read_uint16(v))
            return false;
        value = static_cast<int16_t>(v);
        return true;
    }
    bool read_int32(int32_t& value) {
        uint32_t v = 0;
        if (!read_uint32(v))
            return false;
        value = static_cast<int32_t>(v);
        return true;
    }
    bool read_int64(int64_t& value) {
        uint64_t v = 0;
        if (!read_uint64(v))
            return false;
        value = static_cast<int64_t>(v);
        return true;
    }

    bool read_varint(uint64_t& value);

    bool read_float(float& value);
    bool read_double(double& value);

    bool read_half(float& value);

    bool read_bytes(uint8_t* data, std::size_t len);

    bool read_string(std::string& str);

    template <typename T> bool read_array(std::vector<T>& values);

    // ── State ───────────────────────────────────────────────────────────────

    [[nodiscard]] std::size_t bit_count() const noexcept {
        return write_bit_pos_;
    }
    [[nodiscard]] std::size_t byte_count() const noexcept {
        return (write_bit_pos_ + 7) / 8;
    }
    [[nodiscard]] std::size_t read_bit_pos() const noexcept {
        return read_bit_pos_;
    }

    [[nodiscard]] const uint8_t* data() const noexcept {
        return buffer_.data();
    }
    [[nodiscard]] std::span<const uint8_t> span() const noexcept {
        return std::span<const uint8_t>(buffer_.data(), byte_count());
    }

    void reset() {
        write_bit_pos_ = 0;
        read_bit_pos_ = 0;
        buffer_.clear();
    }

    void reset_read() {
        read_bit_pos_ = 0;
    }

    [[nodiscard]] bool is_aligned_to_byte() const noexcept {
        return (write_bit_pos_ % 8) == 0;
    }
    [[nodiscard]] bool read_aligned_to_byte() const noexcept {
        return (read_bit_pos_ % 8) == 0;
    }

    /// Pad with zero bits until the next byte boundary.
    void align_to_byte() {
        if (write_bit_pos_ % 8 != 0) {
            uint8_t pad = 8 - static_cast<uint8_t>(write_bit_pos_ % 8);
            write_bits(0, pad);
        }
    }

    bool align_read_to_byte() {
        if (read_bit_pos_ % 8 != 0) {
            uint8_t pad = 8 - static_cast<uint8_t>(read_bit_pos_ % 8);
            uint64_t dummy = 0;
            return read_bits(dummy, pad);
        }
        return true;
    }

private:
    std::vector<uint8_t> buffer_;
    std::size_t write_bit_pos_ = 0;
    std::size_t read_bit_pos_ = 0;

    [[nodiscard]] static uint16_t to_net(uint16_t v) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            return v;
        return static_cast<uint16_t>((v >> 8) | (v << 8));
    }
    [[nodiscard]] static uint32_t to_net(uint32_t v) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            return v;
        return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    }
    [[nodiscard]] static uint64_t to_net(uint64_t v) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            return v;
        return (static_cast<uint64_t>(to_net(static_cast<uint32_t>(v))) << 32) | to_net(static_cast<uint32_t>(v >> 32));
    }
};

// ── Inline implementations ──────────────────────────────────────────────────

inline void BitStream::write_bits(uint64_t value, uint8_t num_bits) {
    if (num_bits == 0)
        return;
    for (int i = num_bits - 1; i >= 0; --i) {
        bool bit = (value >> i) & 1u;
        std::size_t byte_idx = write_bit_pos_ / 8;
        std::size_t bit_idx = 7 - (write_bit_pos_ % 8);
        if (byte_idx >= buffer_.size())
            buffer_.push_back(0);
        if (bit)
            buffer_[byte_idx] |= static_cast<uint8_t>(1u << bit_idx);
        ++write_bit_pos_;
    }
}

inline bool BitStream::read_bits(uint64_t& value, uint8_t num_bits) {
    value = 0;
    if (num_bits == 0)
        return true;
    if (read_bit_pos_ + num_bits > write_bit_pos_)
        return false;
    for (int i = 0; i < num_bits; ++i) {
        std::size_t byte_idx = read_bit_pos_ / 8;
        std::size_t bit_idx = 7 - (read_bit_pos_ % 8);
        bool bit = (buffer_[byte_idx] >> bit_idx) & 1u;
        value = (value << 1) | static_cast<uint64_t>(bit);
        ++read_bit_pos_;
    }
    return true;
}

inline void BitStream::write_uint16(uint16_t value) {
    uint16_t net = to_net(value);
    write_bits(net, 16);
}

inline void BitStream::write_uint32(uint32_t value) {
    uint32_t net = to_net(value);
    write_bits(net, 32);
}

inline void BitStream::write_uint64(uint64_t value) {
    uint64_t net = to_net(value);
    write_bits(net, 64);
}

inline void BitStream::write_varint(uint64_t value) {
    align_to_byte();
    while (value > 0x7F) {
        write_uint8(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    write_uint8(static_cast<uint8_t>(value & 0x7F));
}

inline void BitStream::write_float(float value) {
    align_to_byte();
    uint32_t ui = 0;
    static_assert(sizeof(ui) == sizeof(value));
    std::memcpy(&ui, &value, sizeof(value));
    write_uint32(ui);
}

inline void BitStream::write_double(double value) {
    align_to_byte();
    uint64_t ui = 0;
    static_assert(sizeof(ui) == sizeof(value));
    std::memcpy(&ui, &value, sizeof(value));
    write_uint64(ui);
}

inline void BitStream::write_half(float value) {
    align_to_byte();
    uint32_t ui = 0;
    std::memcpy(&ui, &value, sizeof(value));

    uint32_t sign = (ui >> 31) & 0x1;
    uint32_t exponent = (ui >> 23) & 0xFF;
    uint32_t mantissa = ui & 0x7FFFFF;

    uint16_t half = 0;
    if (exponent == 0) {
        half = static_cast<uint16_t>(static_cast<uint32_t>(sign) << 15);
    } else if (exponent == 0xFF) {
        half = static_cast<uint16_t>((static_cast<uint32_t>(sign) << 15) | (0x1Fu << 10) | (mantissa ? 0x200u : 0));
    } else {
        int32_t new_exp = static_cast<int32_t>(exponent) - 127 + 15;
        if (new_exp >= 31) {
            half = static_cast<uint16_t>((static_cast<uint32_t>(sign) << 15) | (0x1Fu << 10));
        } else if (new_exp <= 0) {
            half = static_cast<uint16_t>(static_cast<uint32_t>(sign) << 15);
        } else {
            half = static_cast<uint16_t>((static_cast<uint32_t>(sign) << 15) | (static_cast<uint32_t>(new_exp) << 10) |
                                         static_cast<uint16_t>(mantissa >> 13));
        }
    }
    write_bits(half, 16);
}

inline void BitStream::write_bytes(const uint8_t* data, std::size_t len) {
    align_to_byte();
    std::size_t start_byte = write_bit_pos_ / 8;
    buffer_.resize(start_byte + len);
    std::memcpy(buffer_.data() + start_byte, data, len);
    write_bit_pos_ += len * 8;
}

inline void BitStream::write_string(const std::string& str) {
    write_varint(static_cast<uint64_t>(str.size()));
    write_bytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

inline bool BitStream::read_uint16(uint16_t& value) {
    uint64_t v = 0;
    if (!read_bits(v, 16))
        return false;
    value = to_net(static_cast<uint16_t>(v));
    return true;
}

inline bool BitStream::read_uint32(uint32_t& value) {
    uint64_t v = 0;
    if (!read_bits(v, 32))
        return false;
    value = to_net(static_cast<uint32_t>(v));
    return true;
}

inline bool BitStream::read_uint64(uint64_t& value) {
    uint64_t v = 0;
    if (!read_bits(v, 64))
        return false;
    value = to_net(v);
    return true;
}

inline bool BitStream::read_varint(uint64_t& value) {
    if (!align_read_to_byte())
        return false;
    value = 0;
    uint32_t shift = 0;
    while (true) {
        uint8_t byte = 0;
        if (!read_uint8(byte))
            return false;
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0)
            break;
        shift += 7;
        if (shift >= 64)
            return false;
    }
    return true;
}

inline bool BitStream::read_float(float& value) {
    if (!align_read_to_byte())
        return false;
    uint32_t ui = 0;
    if (!read_uint32(ui))
        return false;
    std::memcpy(&value, &ui, sizeof(value));
    return true;
}

inline bool BitStream::read_double(double& value) {
    if (!align_read_to_byte())
        return false;
    uint64_t ui = 0;
    if (!read_uint64(ui))
        return false;
    std::memcpy(&value, &ui, sizeof(value));
    return true;
}

inline bool BitStream::read_half(float& value) {
    if (!align_read_to_byte())
        return false;
    uint64_t hv = 0;
    if (!read_bits(hv, 16))
        return false;
    uint16_t h = static_cast<uint16_t>(hv);

    uint16_t sign = (h >> 15) & 0x1;
    uint16_t exponent = (h >> 10) & 0x1F;
    uint16_t mantissa = h & 0x3FF;

    uint32_t ui = 0;
    if (exponent == 0) {
        ui = static_cast<uint32_t>(sign) << 31;
    } else if (exponent == 0x1F) {
        ui = (static_cast<uint32_t>(sign) << 31) | (0xFFu << 23) | (static_cast<uint32_t>(mantissa) << 13);
    } else {
        ui = (static_cast<uint32_t>(sign) << 31) | (static_cast<uint32_t>(exponent - 15 + 127) << 23) |
             (static_cast<uint32_t>(mantissa) << 13);
    }
    std::memcpy(&value, &ui, sizeof(value));
    return true;
}

inline bool BitStream::read_bytes(uint8_t* data, std::size_t len) {
    if (!align_read_to_byte())
        return false;
    if (read_bit_pos_ + len * 8 > write_bit_pos_)
        return false;
    std::size_t start_byte = read_bit_pos_ / 8;
    std::memcpy(data, buffer_.data() + start_byte, len);
    read_bit_pos_ += len * 8;
    return true;
}

inline bool BitStream::read_string(std::string& str) {
    uint64_t len = 0;
    if (!read_varint(len))
        return false;
    if (len > 65535)
        return false;
    str.resize(static_cast<std::size_t>(len));
    if (len > 0) {
        if (!read_bytes(reinterpret_cast<uint8_t*>(str.data()), static_cast<std::size_t>(len)))
            return false;
    }
    return true;
}

} // namespace systems::leal::campello_net::serialization

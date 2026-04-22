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

/// @brief Bidirectional bit-level stream for compact binary serialization.
///
/// Bits are packed MSB-first within each byte. Multi-byte values written at
/// byte boundaries are stored in network (big-endian) byte order.
///
/// The stream is dual-mode: it can both write (build a packet) and read
/// (parse a packet) using the same internal buffer.
///
/// Example:
/// @code
/// BitStream stream;
/// stream.write_uint32(42);
/// stream.write_float(3.14f);
/// stream.write_string("hello");
///
/// BitStream reader(stream.span());
/// std::uint32_t i; float f; std::string s;
/// reader.read_uint32(i);
/// reader.read_float(f);
/// reader.read_string(s);
/// @endcode
class BitStream {
public:
    /// @brief Create an empty stream for writing.
    BitStream() = default;

    /// @brief Create a read-only stream from existing data.
    /// @param data The byte buffer to read from.
    explicit BitStream(std::span<const std::uint8_t> data)
        : buffer_(data.begin(), data.end()), write_bit_pos_(buffer_.size() * 8) {}

    // ── Writing ─────────────────────────────────────────────────────────────

    /// @brief Write the lowest @p num_bits of @p value (MSB-first).
    /// @param value The bits to write.
    /// @param num_bits How many bits to write (1–64).
    void write_bits(std::uint64_t value, std::uint8_t num_bits);

    /// @brief Write a single boolean as one bit.
    void write_bool(bool value) {
        write_bits(value ? 1u : 0u, 1);
    }

    /// @brief Write an unsigned 8-bit integer.
    void write_uint8(std::uint8_t value) {
        write_bits(value, 8);
    }
    /// @brief Write an unsigned 16-bit integer in network byte order.
    void write_uint16(std::uint16_t value);
    /// @brief Write an unsigned 32-bit integer in network byte order.
    void write_uint32(std::uint32_t value);
    /// @brief Write an unsigned 64-bit integer in network byte order.
    void write_uint64(std::uint64_t value);

    /// @brief Write a signed 8-bit integer.
    void write_int8(std::int8_t value) {
        write_bits(static_cast<std::uint64_t>(static_cast<std::uint8_t>(value)), 8);
    }
    /// @brief Write a signed 16-bit integer in network byte order.
    void write_int16(std::int16_t value) {
        write_uint16(static_cast<std::uint16_t>(value));
    }
    /// @brief Write a signed 32-bit integer in network byte order.
    void write_int32(std::int32_t value) {
        write_uint32(static_cast<std::uint32_t>(value));
    }
    /// @brief Write a signed 64-bit integer in network byte order.
    void write_int64(std::int64_t value) {
        write_uint64(static_cast<std::uint64_t>(value));
    }

    /// @brief Write a Protobuf-style varint (7 bits per byte, continuation high bit).
    ///
    /// Smaller values use fewer bytes. Values < 128 use 1 byte.
    void write_varint(std::uint64_t value);

    /// @brief Write a full-precision IEEE 754 float.
    void write_float(float value);
    /// @brief Write a full-precision IEEE 754 double.
    void write_double(double value);

    /// @brief Write an IEEE 754 half-precision (16-bit) float.
    ///
    /// Range and precision are reduced compared to full float. Useful for
    /// compact network transmission of normalized values.
    void write_half(float value);

    /// @brief Raw byte copy. The stream must be byte-aligned.
    /// @param data Pointer to bytes to copy.
    /// @param len Number of bytes.
    void write_bytes(const std::uint8_t* data, std::size_t len);

    /// @brief Write a length-prefixed UTF-8 string.
    ///
    /// Length is encoded as a varint, followed by the raw bytes.
    void write_string(const std::string& str);

    /// @brief Write a span of trivially-copyable values.
    /// @tparam T Element type.
    /// @note Not yet implemented.
    template <typename T> void write_array(std::span<const T> values);

    // ── Reading ─────────────────────────────────────────────────────────────

    /// @brief Read @p num_bits into @p value (MSB-first).
    /// @return false if the stream does not contain enough bits.
    bool read_bits(std::uint64_t& value, std::uint8_t num_bits);

    /// @brief Read a single boolean from one bit.
    /// @return false on overflow.
    bool read_bool(bool& value) {
        std::uint64_t v = 0;
        if (!read_bits(v, 1))
            return false;
        value = v != 0;
        return true;
    }

    /// @brief Read an unsigned 8-bit integer.
    /// @return false on overflow.
    bool read_uint8(std::uint8_t& value) {
        std::uint64_t v = 0;
        if (!read_bits(v, 8))
            return false;
        value = static_cast<std::uint8_t>(v);
        return true;
    }
    /// @brief Read an unsigned 16-bit integer in network byte order.
    /// @return false on overflow.
    bool read_uint16(std::uint16_t& value);
    /// @brief Read an unsigned 32-bit integer in network byte order.
    /// @return false on overflow.
    bool read_uint32(std::uint32_t& value);
    /// @brief Read an unsigned 64-bit integer in network byte order.
    /// @return false on overflow.
    bool read_uint64(std::uint64_t& value);

    /// @brief Read a signed 8-bit integer.
    /// @return false on overflow.
    bool read_int8(std::int8_t& value) {
        std::uint8_t v = 0;
        if (!read_uint8(v))
            return false;
        value = static_cast<std::int8_t>(v);
        return true;
    }
    /// @brief Read a signed 16-bit integer in network byte order.
    /// @return false on overflow.
    bool read_int16(std::int16_t& value) {
        std::uint16_t v = 0;
        if (!read_uint16(v))
            return false;
        value = static_cast<std::int16_t>(v);
        return true;
    }
    /// @brief Read a signed 32-bit integer in network byte order.
    /// @return false on overflow.
    bool read_int32(std::int32_t& value) {
        std::uint32_t v = 0;
        if (!read_uint32(v))
            return false;
        value = static_cast<std::int32_t>(v);
        return true;
    }
    /// @brief Read a signed 64-bit integer in network byte order.
    /// @return false on overflow.
    bool read_int64(std::int64_t& value) {
        std::uint64_t v = 0;
        if (!read_uint64(v))
            return false;
        value = static_cast<std::int64_t>(v);
        return true;
    }

    /// @brief Read a Protobuf-style varint.
    /// @return false on overflow or malformed continuation.
    bool read_varint(std::uint64_t& value);

    /// @brief Read a full-precision IEEE 754 float.
    /// @return false on overflow.
    bool read_float(float& value);
    /// @brief Read a full-precision IEEE 754 double.
    /// @return false on overflow.
    bool read_double(double& value);

    /// @brief Read an IEEE 754 half-precision (16-bit) float.
    /// @return false on overflow.
    bool read_half(float& value);

    /// @brief Raw byte copy. The stream must be byte-aligned.
    /// @return false on overflow or misalignment.
    bool read_bytes(std::uint8_t* data, std::size_t len);

    /// @brief Read a length-prefixed UTF-8 string.
    /// @return false on overflow or if length exceeds 65535.
    bool read_string(std::string& str);

    /// @brief Read a vector of trivially-copyable values.
    /// @tparam T Element type.
    /// @note Not yet implemented.
    template <typename T> bool read_array(std::vector<T>& values);

    // ── State queries ───────────────────────────────────────────────────────

    /// @return Total number of bits written.
    [[nodiscard]] std::size_t bit_count() const noexcept {
        return write_bit_pos_;
    }
    /// @return Total number of bytes written (ceil of bit_count / 8).
    [[nodiscard]] std::size_t byte_count() const noexcept {
        return (write_bit_pos_ + 7) / 8;
    }
    /// @return Current read position in bits.
    [[nodiscard]] std::size_t read_bit_pos() const noexcept {
        return read_bit_pos_;
    }

    /// @return Pointer to the underlying byte buffer.
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return buffer_.data();
    }
    /// @return A span view of the valid written bytes.
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept {
        return std::span<const std::uint8_t>(buffer_.data(), byte_count());
    }

    // ── State manipulation ──────────────────────────────────────────────────

    /// @brief Reset both write and read positions and clear the buffer.
    void reset() {
        write_bit_pos_ = 0;
        read_bit_pos_ = 0;
        buffer_.clear();
    }

    /// @brief Reset only the read position to the start.
    void reset_read() {
        read_bit_pos_ = 0;
    }

    /// @return true if the write cursor is on a byte boundary.
    [[nodiscard]] bool is_aligned_to_byte() const noexcept {
        return (write_bit_pos_ % 8) == 0;
    }
    /// @return true if the read cursor is on a byte boundary.
    [[nodiscard]] bool read_aligned_to_byte() const noexcept {
        return (read_bit_pos_ % 8) == 0;
    }

    /// @brief Pad with zero bits until the next byte boundary.
    void align_to_byte() {
        if (write_bit_pos_ % 8 != 0) {
            std::uint8_t pad = 8 - static_cast<std::uint8_t>(write_bit_pos_ % 8);
            write_bits(0, pad);
        }
    }

    /// @brief Advance the read cursor to the next byte boundary.
    /// @return false if there are not enough bits remaining.
    bool align_read_to_byte() {
        if (read_bit_pos_ % 8 != 0) {
            std::uint8_t pad = 8 - static_cast<std::uint8_t>(read_bit_pos_ % 8);
            std::uint64_t dummy = 0;
            return read_bits(dummy, pad);
        }
        return true;
    }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t write_bit_pos_ = 0;
    std::size_t read_bit_pos_ = 0;

    [[nodiscard]] static std::uint16_t to_net(std::uint16_t v) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            return v;
        return static_cast<std::uint16_t>((v >> 8) | (v << 8));
    }
    [[nodiscard]] static std::uint32_t to_net(std::uint32_t v) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            return v;
        return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    }
    [[nodiscard]] static std::uint64_t to_net(std::uint64_t v) noexcept {
        if constexpr (std::endian::native == std::endian::big)
            return v;
        return (static_cast<std::uint64_t>(to_net(static_cast<std::uint32_t>(v))) << 32) | to_net(static_cast<std::uint32_t>(v >> 32));
    }
};

// ── Inline implementations ──────────────────────────────────────────────────

inline void BitStream::write_bits(std::uint64_t value, std::uint8_t num_bits) {
    if (num_bits == 0)
        return;
    for (int i = num_bits - 1; i >= 0; --i) {
        bool bit = (value >> i) & 1u;
        std::size_t byte_idx = write_bit_pos_ / 8;
        std::size_t bit_idx = 7 - (write_bit_pos_ % 8);
        if (byte_idx >= buffer_.size())
            buffer_.push_back(0);
        if (bit)
            buffer_[byte_idx] |= static_cast<std::uint8_t>(1u << bit_idx);
        ++write_bit_pos_;
    }
}

inline bool BitStream::read_bits(std::uint64_t& value, std::uint8_t num_bits) {
    value = 0;
    if (num_bits == 0)
        return true;
    if (read_bit_pos_ + num_bits > write_bit_pos_)
        return false;
    for (int i = 0; i < num_bits; ++i) {
        std::size_t byte_idx = read_bit_pos_ / 8;
        std::size_t bit_idx = 7 - (read_bit_pos_ % 8);
        bool bit = (buffer_[byte_idx] >> bit_idx) & 1u;
        value = (value << 1) | static_cast<std::uint64_t>(bit);
        ++read_bit_pos_;
    }
    return true;
}

inline void BitStream::write_uint16(std::uint16_t value) {
    std::uint16_t net = to_net(value);
    write_bits(net, 16);
}

inline void BitStream::write_uint32(std::uint32_t value) {
    std::uint32_t net = to_net(value);
    write_bits(net, 32);
}

inline void BitStream::write_uint64(std::uint64_t value) {
    std::uint64_t net = to_net(value);
    write_bits(net, 64);
}

inline void BitStream::write_varint(std::uint64_t value) {
    align_to_byte();
    while (value > 0x7F) {
        write_uint8(static_cast<std::uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    write_uint8(static_cast<std::uint8_t>(value & 0x7F));
}

inline void BitStream::write_float(float value) {
    align_to_byte();
    std::uint32_t ui = 0;
    static_assert(sizeof(ui) == sizeof(value));
    std::memcpy(&ui, &value, sizeof(value));
    write_uint32(ui);
}

inline void BitStream::write_double(double value) {
    align_to_byte();
    std::uint64_t ui = 0;
    static_assert(sizeof(ui) == sizeof(value));
    std::memcpy(&ui, &value, sizeof(value));
    write_uint64(ui);
}

inline void BitStream::write_half(float value) {
    align_to_byte();
    std::uint32_t ui = 0;
    std::memcpy(&ui, &value, sizeof(value));

    std::uint32_t sign = (ui >> 31) & 0x1;
    std::uint32_t exponent = (ui >> 23) & 0xFF;
    std::uint32_t mantissa = ui & 0x7FFFFF;

    std::uint16_t half = 0;
    if (exponent == 0) {
        half = static_cast<std::uint16_t>(static_cast<std::uint32_t>(sign) << 15);
    } else if (exponent == 0xFF) {
        half = static_cast<std::uint16_t>((static_cast<std::uint32_t>(sign) << 15) | (0x1Fu << 10) | (mantissa ? 0x200u : 0));
    } else {
        std::int32_t new_exp = static_cast<std::int32_t>(exponent) - 127 + 15;
        if (new_exp >= 31) {
            half = static_cast<std::uint16_t>((static_cast<std::uint32_t>(sign) << 15) | (0x1Fu << 10));
        } else if (new_exp <= 0) {
            half = static_cast<std::uint16_t>(static_cast<std::uint32_t>(sign) << 15);
        } else {
            half = static_cast<std::uint16_t>((static_cast<std::uint32_t>(sign) << 15) | (static_cast<std::uint32_t>(new_exp) << 10) |
                                         static_cast<std::uint16_t>(mantissa >> 13));
        }
    }
    write_bits(half, 16);
}

inline void BitStream::write_bytes(const std::uint8_t* data, std::size_t len) {
    align_to_byte();
    std::size_t start_byte = write_bit_pos_ / 8;
    buffer_.resize(start_byte + len);
    std::memcpy(buffer_.data() + start_byte, data, len);
    write_bit_pos_ += len * 8;
}

inline void BitStream::write_string(const std::string& str) {
    write_varint(static_cast<std::uint64_t>(str.size()));
    write_bytes(reinterpret_cast<const std::uint8_t*>(str.data()), str.size());
}

inline bool BitStream::read_uint16(std::uint16_t& value) {
    std::uint64_t v = 0;
    if (!read_bits(v, 16))
        return false;
    value = to_net(static_cast<std::uint16_t>(v));
    return true;
}

inline bool BitStream::read_uint32(std::uint32_t& value) {
    std::uint64_t v = 0;
    if (!read_bits(v, 32))
        return false;
    value = to_net(static_cast<std::uint32_t>(v));
    return true;
}

inline bool BitStream::read_uint64(std::uint64_t& value) {
    std::uint64_t v = 0;
    if (!read_bits(v, 64))
        return false;
    value = to_net(v);
    return true;
}

inline bool BitStream::read_varint(std::uint64_t& value) {
    if (!align_read_to_byte())
        return false;
    value = 0;
    std::uint32_t shift = 0;
    while (true) {
        std::uint8_t byte = 0;
        if (!read_uint8(byte))
            return false;
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
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
    std::uint32_t ui = 0;
    if (!read_uint32(ui))
        return false;
    std::memcpy(&value, &ui, sizeof(value));
    return true;
}

inline bool BitStream::read_double(double& value) {
    if (!align_read_to_byte())
        return false;
    std::uint64_t ui = 0;
    if (!read_uint64(ui))
        return false;
    std::memcpy(&value, &ui, sizeof(value));
    return true;
}

inline bool BitStream::read_half(float& value) {
    if (!align_read_to_byte())
        return false;
    std::uint64_t hv = 0;
    if (!read_bits(hv, 16))
        return false;
    std::uint16_t h = static_cast<std::uint16_t>(hv);

    std::uint16_t sign = (h >> 15) & 0x1;
    std::uint16_t exponent = (h >> 10) & 0x1F;
    std::uint16_t mantissa = h & 0x3FF;

    std::uint32_t ui = 0;
    if (exponent == 0) {
        ui = static_cast<std::uint32_t>(sign) << 31;
    } else if (exponent == 0x1F) {
        ui = (static_cast<std::uint32_t>(sign) << 31) | (0xFFu << 23) | (static_cast<std::uint32_t>(mantissa) << 13);
    } else {
        ui = (static_cast<std::uint32_t>(sign) << 31) | (static_cast<std::uint32_t>(exponent - 15 + 127) << 23) |
             (static_cast<std::uint32_t>(mantissa) << 13);
    }
    std::memcpy(&value, &ui, sizeof(value));
    return true;
}

inline bool BitStream::read_bytes(std::uint8_t* data, std::size_t len) {
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
    std::uint64_t len = 0;
    if (!read_varint(len))
        return false;
    if (len > 65535)
        return false;
    str.resize(static_cast<std::size_t>(len));
    if (len > 0) {
        if (!read_bytes(reinterpret_cast<std::uint8_t*>(str.data()), static_cast<std::size_t>(len)))
            return false;
    }
    return true;
}

// ── Template definitions (must follow class body) ───────────────────────────

template <typename T>
inline void BitStream::write_array(std::span<const T> values) {
    write_varint(static_cast<std::uint64_t>(values.size()));
    for (const auto& v : values) {
        serialize(*this, v);
    }
}

template <typename T>
inline bool BitStream::read_array(std::vector<T>& values) {
    std::uint64_t len = 0;
    if (!read_varint(len))
        return false;
    if (len > 1000000)
        return false; // sanity limit
    values.resize(static_cast<std::size_t>(len));
    for (auto& v : values) {
        if (!deserialize(*this, v))
            return false;
    }
    return true;
}

} // namespace systems::leal::campello_net::serialization

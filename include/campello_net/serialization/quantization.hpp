#pragma once

#include "campello_net/serialization/bit_stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace systems::leal::campello_net::serialization {

/// @brief Quantize a float into an unsigned integer range and write it.
///
/// The value is clamped to [@p min, @p max], normalized to [0, 1], then
/// scaled to fit in @p bits. Use this for compact transmission of
/// bounded scalar values (e.g., health percentages, normalized angles).
///
/// @param stream Destination bit stream.
/// @param value Float value to quantize.
/// @param min Lower bound of the quantization range.
/// @param max Upper bound of the quantization range.
/// @param bits Number of bits to use (1–32).
inline void write_quantized(BitStream& stream, float value, float min, float max, std::uint8_t bits) {
    if (bits == 0)
        return;
    if (bits > 32)
        bits = 32;
    std::uint32_t max_int = (1u << bits) - 1;
    float t = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
    std::uint32_t q = static_cast<std::uint32_t>(t * static_cast<float>(max_int) + 0.5f);
    stream.write_bits(q, bits);
}

/// @brief Read a quantized float from the stream.
/// @return false if the stream does not contain enough bits.
inline bool read_quantized(BitStream& stream, float& value, float min, float max, std::uint8_t bits) {
    if (bits == 0) {
        value = min;
        return true;
    }
    if (bits > 32)
        bits = 32;
    std::uint64_t q = 0;
    if (!stream.read_bits(q, bits))
        return false;
    std::uint32_t max_int = (1u << bits) - 1;
    value = min + (max - min) * (static_cast<float>(q) / static_cast<float>(max_int));
    return true;
}

/// @brief Serialize a unit quaternion using smallest-three encoding.
///
/// Stores a 2-bit index for the dropped (largest) component plus 3
/// quantized components. This reduces a quaternion from 128 bits to
/// 2 + 3×@p bits_per_component bits with minimal visual error.
///
/// @param stream Destination bit stream.
/// @param quat A unit quaternion [x, y, z, w].
/// @param bits_per_component Quantization bits per stored component.
inline void write_smallest_three(BitStream& stream, const std::array<float, 4>& quat, std::uint8_t bits_per_component) {
    std::size_t largest_idx = 0;
    float largest_abs = std::abs(quat[0]);
    for (std::size_t i = 1; i < 4; ++i) {
        if (std::abs(quat[i]) > largest_abs) {
            largest_abs = std::abs(quat[i]);
            largest_idx = i;
        }
    }
    stream.write_bits(static_cast<std::uint64_t>(largest_idx), 2);

    for (std::size_t i = 0; i < 4; ++i) {
        if (i == largest_idx)
            continue;
        write_quantized(stream, quat[i], -1.0f, 1.0f, bits_per_component);
    }
}

/// @brief Deserialize a unit quaternion from smallest-three encoding.
/// @return false on read failure.
/// @note The reconstructed quaternion may deviate slightly from unit length;
/// callers should normalize before use.
inline bool read_smallest_three(BitStream& stream, std::array<float, 4>& quat, std::uint8_t bits_per_component) {
    std::uint64_t largest_idx = 0;
    if (!stream.read_bits(largest_idx, 2))
        return false;

    for (std::size_t i = 0; i < 4; ++i) {
        if (largest_idx == i)
            continue;
        if (!read_quantized(stream, quat[i], -1.0f, 1.0f, bits_per_component))
            return false;
    }

    float sum_sq = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        if (largest_idx != i)
            sum_sq += quat[i] * quat[i];
    }
    float reconstructed = std::sqrt(std::max(0.0f, 1.0f - sum_sq));
    quat[largest_idx] = reconstructed;
    return true;
}

} // namespace systems::leal::campello_net::serialization

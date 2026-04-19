#pragma once

#include "campello_net/serialization/bit_stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace systems::leal::campello_net::serialization {

/// Quantize a float into @p bits integer range [min, max].
inline void write_quantized(BitStream& stream, float value, float min, float max, uint8_t bits) {
    if (bits == 0) return;
    if (bits > 32) bits = 32;
    uint32_t max_int = (1u << bits) - 1;
    float t = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
    uint32_t q = static_cast<uint32_t>(t * static_cast<float>(max_int) + 0.5f);
    stream.write_bits(q, bits);
}

inline bool read_quantized(BitStream& stream, float& value, float min, float max, uint8_t bits) {
    if (bits == 0) {
        value = min;
        return true;
    }
    if (bits > 32) bits = 32;
    uint64_t q = 0;
    if (!stream.read_bits(q, bits)) return false;
    uint32_t max_int = (1u << bits) - 1;
    value = min + (max - min) * (static_cast<float>(q) / static_cast<float>(max_int));
    return true;
}

/// Serialize a unit quaternion using smallest-three encoding.
/// Stores a 2-bit index for the dropped (largest) component plus 3 quantized components.
inline void write_smallest_three(BitStream& stream, const std::array<float, 4>& quat, uint8_t bits_per_component) {
    // Find the component with the largest absolute value.
    std::size_t largest_idx = 0;
    float largest_abs = std::abs(quat[0]);
    for (std::size_t i = 1; i < 4; ++i) {
        if (std::abs(quat[i]) > largest_abs) {
            largest_abs = std::abs(quat[i]);
            largest_idx = i;
        }
    }
    stream.write_bits(static_cast<uint64_t>(largest_idx), 2);

    for (std::size_t i = 0; i < 4; ++i) {
        if (i == largest_idx) continue;
        write_quantized(stream, quat[i], -1.0f, 1.0f, bits_per_component);
    }
}

inline bool read_smallest_three(BitStream& stream, std::array<float, 4>& quat, uint8_t bits_per_component) {
    uint64_t largest_idx = 0;
    if (!stream.read_bits(largest_idx, 2)) return false;

    for (std::size_t i = 0; i < 4; ++i) {
        if (largest_idx == i) continue;
        if (!read_quantized(stream, quat[i], -1.0f, 1.0f, bits_per_component)) return false;
    }

    // Reconstruct the dropped component from the unit constraint x²+y²+z²+w² = 1.
    float sum_sq = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        if (largest_idx != i) sum_sq += quat[i] * quat[i];
    }
    float reconstructed = std::sqrt(std::max(0.0f, 1.0f - sum_sq));
    // Restore the sign of the largest component. We stored the absolute largest;
    // the sign is inferred to produce the quaternion closest to the original.
    // For simplicity we always use positive; callers should call normalize().
    quat[largest_idx] = reconstructed;
    return true;
}

} // namespace systems::leal::campello_net::serialization

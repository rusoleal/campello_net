#pragma once

#include "campello_net/serialization/bit_stream.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace systems::leal::campello_net::serialization {

/// Concept for types that provide member serialize/deserialize.
template <typename T>
concept MemberSerializable = requires(T& t, const T& ct, BitStream& stream) {
    { ct.serialize(stream) } -> std::same_as<void>;
    { t.deserialize(stream) } -> std::same_as<bool>;
};

// ── Generic serialize / deserialize overloads ───────────────────────────────

inline void serialize(BitStream& stream, bool value) {
    stream.write_bool(value);
}
inline bool deserialize(BitStream& stream, bool& value) {
    return stream.read_bool(value);
}

inline void serialize(BitStream& stream, uint8_t value) {
    stream.write_uint8(value);
}
inline bool deserialize(BitStream& stream, uint8_t& value) {
    return stream.read_uint8(value);
}

inline void serialize(BitStream& stream, uint16_t value) {
    stream.write_uint16(value);
}
inline bool deserialize(BitStream& stream, uint16_t& value) {
    return stream.read_uint16(value);
}

inline void serialize(BitStream& stream, uint32_t value) {
    stream.write_uint32(value);
}
inline bool deserialize(BitStream& stream, uint32_t& value) {
    return stream.read_uint32(value);
}

inline void serialize(BitStream& stream, uint64_t value) {
    stream.write_uint64(value);
}
inline bool deserialize(BitStream& stream, uint64_t& value) {
    return stream.read_uint64(value);
}

inline void serialize(BitStream& stream, int8_t value) {
    stream.write_uint8(static_cast<uint8_t>(value));
}
inline bool deserialize(BitStream& stream, int8_t& value) {
    uint8_t v = 0;
    if (!stream.read_uint8(v))
        return false;
    value = static_cast<int8_t>(v);
    return true;
}

inline void serialize(BitStream& stream, int16_t value) {
    stream.write_uint16(static_cast<uint16_t>(value));
}
inline bool deserialize(BitStream& stream, int16_t& value) {
    uint16_t v = 0;
    if (!stream.read_uint16(v))
        return false;
    value = static_cast<int16_t>(v);
    return true;
}

inline void serialize(BitStream& stream, int32_t value) {
    stream.write_uint32(static_cast<uint32_t>(value));
}
inline bool deserialize(BitStream& stream, int32_t& value) {
    uint32_t v = 0;
    if (!stream.read_uint32(v))
        return false;
    value = static_cast<int32_t>(v);
    return true;
}

inline void serialize(BitStream& stream, int64_t value) {
    stream.write_uint64(static_cast<uint64_t>(value));
}
inline bool deserialize(BitStream& stream, int64_t& value) {
    uint64_t v = 0;
    if (!stream.read_uint64(v))
        return false;
    value = static_cast<int64_t>(v);
    return true;
}

inline void serialize(BitStream& stream, float value) {
    stream.write_float(value);
}
inline bool deserialize(BitStream& stream, float& value) {
    return stream.read_float(value);
}

inline void serialize(BitStream& stream, double value) {
    stream.write_double(value);
}
inline bool deserialize(BitStream& stream, double& value) {
    return stream.read_double(value);
}

inline void serialize(BitStream& stream, const std::string& value) {
    stream.write_string(value);
}
inline bool deserialize(BitStream& stream, std::string& value) {
    return stream.read_string(value);
}

template <typename T, std::size_t N>
    requires(std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
inline void serialize(BitStream& stream, const std::array<T, N>& value) {
    for (const auto& elem : value) {
        serialize(stream, elem);
    }
}

template <typename T, std::size_t N>
    requires(std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
inline bool deserialize(BitStream& stream, std::array<T, N>& value) {
    for (auto& elem : value) {
        if (!deserialize(stream, elem))
            return false;
    }
    return true;
}

template <typename T>
    requires(std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
inline void serialize(BitStream& stream, const std::vector<T>& value) {
    stream.write_varint(static_cast<uint64_t>(value.size()));
    for (const auto& elem : value) {
        serialize(stream, elem);
    }
}

template <typename T>
    requires(std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
inline bool deserialize(BitStream& stream, std::vector<T>& value) {
    uint64_t size = 0;
    if (!stream.read_varint(size))
        return false;
    if (size > 1000000)
        return false; // sanity limit
    value.resize(static_cast<std::size_t>(size));
    for (auto& elem : value) {
        if (!deserialize(stream, elem))
            return false;
    }
    return true;
}

// Member-serializable types
template <MemberSerializable T> inline void serialize(BitStream& stream, const T& value) {
    value.serialize(stream);
}

template <MemberSerializable T> inline bool deserialize(BitStream& stream, T& value) {
    return value.deserialize(stream);
}

} // namespace systems::leal::campello_net::serialization

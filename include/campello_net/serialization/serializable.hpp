#pragma once

#include "campello_net/serialization/bit_stream.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace systems::leal::campello_net::serialization {

/// @brief Concept for types that provide member serialize/deserialize.
///
/// A type satisfies this concept if it implements:
/// @code
///   void serialize(BitStream& stream) const;
///   bool deserialize(BitStream& stream);
/// @endcode
///
/// Example:
/// @code
/// struct PlayerState {
///     float x = 0, y = 0;
///     void serialize(BitStream& s) const {
///         s.write_float(x);
///         s.write_float(y);
///     }
///     bool deserialize(BitStream& s) {
///         return s.read_float(x) && s.read_float(y);
///     }
/// };
/// @endcode
template <typename T>
concept MemberSerializable = requires(T& t, const T& ct, BitStream& stream) {
    { ct.serialize(stream) } -> std::same_as<void>;
    { t.deserialize(stream) } -> std::same_as<bool>;
};

// ── Primitive serialize / deserialize overloads ─────────────────────────────

/// @name Boolean
/// @{
inline void serialize(BitStream& stream, bool value) {
    stream.write_bool(value);
}
inline bool deserialize(BitStream& stream, bool& value) {
    return stream.read_bool(value);
}
/// @}

/// @name 8-bit unsigned integer
/// @{
inline void serialize(BitStream& stream, std::uint8_t value) {
    stream.write_uint8(value);
}
inline bool deserialize(BitStream& stream, std::uint8_t& value) {
    return stream.read_uint8(value);
}
/// @}

/// @name 16-bit unsigned integer
/// @{
inline void serialize(BitStream& stream, std::uint16_t value) {
    stream.write_uint16(value);
}
inline bool deserialize(BitStream& stream, std::uint16_t& value) {
    return stream.read_uint16(value);
}
/// @}

/// @name 32-bit unsigned integer
/// @{
inline void serialize(BitStream& stream, std::uint32_t value) {
    stream.write_uint32(value);
}
inline bool deserialize(BitStream& stream, std::uint32_t& value) {
    return stream.read_uint32(value);
}
/// @}

/// @name 64-bit unsigned integer
/// @{
inline void serialize(BitStream& stream, std::uint64_t value) {
    stream.write_uint64(value);
}
inline bool deserialize(BitStream& stream, std::uint64_t& value) {
    return stream.read_uint64(value);
}
/// @}

/// @name 8-bit signed integer
/// @{
inline void serialize(BitStream& stream, std::int8_t value) {
    stream.write_uint8(static_cast<std::uint8_t>(value));
}
inline bool deserialize(BitStream& stream, std::int8_t& value) {
    std::uint8_t v = 0;
    if (!stream.read_uint8(v))
        return false;
    value = static_cast<std::int8_t>(v);
    return true;
}
/// @}

/// @name 16-bit signed integer
/// @{
inline void serialize(BitStream& stream, std::int16_t value) {
    stream.write_uint16(static_cast<std::uint16_t>(value));
}
inline bool deserialize(BitStream& stream, std::int16_t& value) {
    std::uint16_t v = 0;
    if (!stream.read_uint16(v))
        return false;
    value = static_cast<std::int16_t>(v);
    return true;
}
/// @}

/// @name 32-bit signed integer
/// @{
inline void serialize(BitStream& stream, std::int32_t value) {
    stream.write_uint32(static_cast<std::uint32_t>(value));
}
inline bool deserialize(BitStream& stream, std::int32_t& value) {
    std::uint32_t v = 0;
    if (!stream.read_uint32(v))
        return false;
    value = static_cast<std::int32_t>(v);
    return true;
}
/// @}

/// @name 64-bit signed integer
/// @{
inline void serialize(BitStream& stream, std::int64_t value) {
    stream.write_uint64(static_cast<std::uint64_t>(value));
}
inline bool deserialize(BitStream& stream, std::int64_t& value) {
    std::uint64_t v = 0;
    if (!stream.read_uint64(v))
        return false;
    value = static_cast<std::int64_t>(v);
    return true;
}
/// @}

/// @name Single-precision float
/// @{
inline void serialize(BitStream& stream, float value) {
    stream.write_float(value);
}
inline bool deserialize(BitStream& stream, float& value) {
    return stream.read_float(value);
}
/// @}

/// @name Double-precision float
/// @{
inline void serialize(BitStream& stream, double value) {
    stream.write_double(value);
}
inline bool deserialize(BitStream& stream, double& value) {
    return stream.read_double(value);
}
/// @}

/// @name String
/// @{
inline void serialize(BitStream& stream, const std::string& value) {
    stream.write_string(value);
}
inline bool deserialize(BitStream& stream, std::string& value) {
    return stream.read_string(value);
}
/// @}

/// @name Fixed-size array
/// @brief Serializes each element in order using the element type's overload.
/// @tparam T Element type (must be trivially copyable and not bool).
/// @tparam N Array size.
/// @{
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
/// @}

/// @name Variable-length vector
/// @brief Prefixes the element count as a varint, then serializes each element.
/// @tparam T Element type (must be trivially copyable and not bool).
/// @note A hard sanity limit of 1,000,000 elements is enforced on deserialize.
/// @{
template <typename T>
    requires(std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
inline void serialize(BitStream& stream, const std::vector<T>& value) {
    stream.write_varint(static_cast<std::uint64_t>(value.size()));
    for (const auto& elem : value) {
        serialize(stream, elem);
    }
}

template <typename T>
    requires(std::is_trivially_copyable_v<T> && !std::is_same_v<T, bool>)
inline bool deserialize(BitStream& stream, std::vector<T>& value) {
    std::uint64_t size = 0;
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
/// @}

/// @name Member-serializable types
/// @brief Delegates to the type's own serialize()/deserialize() members.
/// @{
template <MemberSerializable T> inline void serialize(BitStream& stream, const T& value) {
    value.serialize(stream);
}

template <MemberSerializable T> inline bool deserialize(BitStream& stream, T& value) {
    return value.deserialize(stream);
}
/// @}

} // namespace systems::leal::campello_net::serialization

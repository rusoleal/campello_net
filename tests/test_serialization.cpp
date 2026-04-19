#include <campello_net/serialization/bit_stream.hpp>
#include <campello_net/serialization/quantization.hpp>
#include <campello_net/serialization/serializable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

using namespace systems::leal::campello_net::serialization;

TEST_CASE("BitStream basic bit read/write", "[serialization]") {
    BitStream stream;
    stream.write_bits(0b101, 3);
    stream.write_bits(0b01, 2);
    stream.write_bool(true);
    stream.write_bool(false);

    REQUIRE(stream.bit_count() == 7);

    stream.reset_read();
    uint64_t v = 0;
    REQUIRE(stream.read_bits(v, 3));
    REQUIRE(v == 0b101);
    REQUIRE(stream.read_bits(v, 2));
    REQUIRE(v == 0b01);
    bool b = false;
    REQUIRE(stream.read_bool(b));
    REQUIRE(b == true);
    REQUIRE(stream.read_bool(b));
    REQUIRE(b == false);
}

TEST_CASE("BitStream integer roundtrip", "[serialization]") {
    BitStream stream;
    stream.write_uint8(42);
    stream.write_uint16(12345);
    stream.write_uint32(0xDEADBEEF);
    stream.write_uint64(0x123456789ABCDEF0);

    stream.reset_read();
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    REQUIRE(stream.read_uint8(u8));
    REQUIRE(u8 == 42);
    REQUIRE(stream.read_uint16(u16));
    REQUIRE(u16 == 12345);
    REQUIRE(stream.read_uint32(u32));
    REQUIRE(u32 == 0xDEADBEEF);
    REQUIRE(stream.read_uint64(u64));
    REQUIRE(u64 == 0x123456789ABCDEF0);
}

TEST_CASE("BitStream signed integer roundtrip", "[serialization]") {
    BitStream stream;
    stream.write_int8(-5);
    stream.write_int16(-1000);
    stream.write_int32(-50000);
    stream.write_int64(-123456789);

    stream.reset_read();
    int8_t i8 = 0;
    int16_t i16 = 0;
    int32_t i32 = 0;
    int64_t i64 = 0;
    REQUIRE(stream.read_int8(i8));
    REQUIRE(i8 == -5);
    REQUIRE(stream.read_int16(i16));
    REQUIRE(i16 == -1000);
    REQUIRE(stream.read_int32(i32));
    REQUIRE(i32 == -50000);
    REQUIRE(stream.read_int64(i64));
    REQUIRE(i64 == -123456789);
}

TEST_CASE("BitStream varint roundtrip", "[serialization]") {
    BitStream stream;
    stream.write_varint(0);
    stream.write_varint(1);
    stream.write_varint(127);
    stream.write_varint(128);
    stream.write_varint(16383);
    stream.write_varint(16384);
    stream.write_varint(123456789);

    stream.reset_read();
    uint64_t v = 0;
    REQUIRE(stream.read_varint(v));
    REQUIRE(v == 0);
    REQUIRE(stream.read_varint(v));
    REQUIRE(v == 1);
    REQUIRE(stream.read_varint(v));
    REQUIRE(v == 127);
    REQUIRE(stream.read_varint(v));
    REQUIRE(v == 128);
    REQUIRE(stream.read_varint(v));
    REQUIRE(v == 16383);
    REQUIRE(stream.read_varint(v));
    REQUIRE(v == 16384);
    REQUIRE(stream.read_varint(v));
    REQUIRE(v == 123456789);
}

TEST_CASE("BitStream float and double roundtrip", "[serialization]") {
    BitStream stream;
    stream.write_float(3.14159f);
    stream.write_double(2.718281828459045);

    stream.reset_read();
    float f = 0.0f;
    double d = 0.0;
    REQUIRE(stream.read_float(f));
    REQUIRE(f == 3.14159f);
    REQUIRE(stream.read_double(d));
    REQUIRE(d == 2.718281828459045);
}

TEST_CASE("BitStream half-precision float roundtrip", "[serialization]") {
    BitStream stream;
    stream.write_half(1.0f);
    stream.write_half(-1.0f);
    stream.write_half(0.0f);
    stream.write_half(65504.0f); // max half
    stream.write_half(-65504.0f);
    stream.write_half(3.14159f); // will lose precision

    stream.reset_read();
    float f = 0.0f;
    REQUIRE(stream.read_half(f));
    REQUIRE(f == 1.0f);
    REQUIRE(stream.read_half(f));
    REQUIRE(f == -1.0f);
    REQUIRE(stream.read_half(f));
    REQUIRE(f == 0.0f);
    REQUIRE(stream.read_half(f));
    REQUIRE(f == 65504.0f);
    REQUIRE(stream.read_half(f));
    REQUIRE(f == -65504.0f);
    REQUIRE(stream.read_half(f));
    REQUIRE(std::abs(f - 3.14159f) < 0.01f);
}

TEST_CASE("BitStream string roundtrip", "[serialization]") {
    BitStream stream;
    stream.write_string("hello");
    stream.write_string("");
    stream.write_string("campello_net serialization");

    stream.reset_read();
    std::string s;
    REQUIRE(stream.read_string(s));
    REQUIRE(s == "hello");
    REQUIRE(stream.read_string(s));
    REQUIRE(s.empty());
    REQUIRE(stream.read_string(s));
    REQUIRE(s == "campello_net serialization");
}

TEST_CASE("BitStream vector roundtrip", "[serialization]") {
    BitStream stream;
    std::vector<uint32_t> original = {1, 2, 3, 100, 5000};
    serialize(stream, original);

    stream.reset_read();
    std::vector<uint32_t> decoded;
    REQUIRE(deserialize(stream, decoded));
    REQUIRE(decoded == original);
}

TEST_CASE("BitStream array roundtrip", "[serialization]") {
    BitStream stream;
    std::array<float, 4> original = {1.0f, 2.0f, 3.0f, 4.0f};
    serialize(stream, original);

    stream.reset_read();
    std::array<float, 4> decoded{};
    REQUIRE(deserialize(stream, decoded));
    REQUIRE(decoded == original);
}

TEST_CASE("Quantized float roundtrip", "[serialization]") {
    BitStream stream;
    write_quantized(stream, 50.0f, 0.0f, 100.0f, 10);
    write_quantized(stream, 0.0f, -100.0f, 100.0f, 12);
    write_quantized(stream, -50.0f, -100.0f, 100.0f, 8);

    stream.reset_read();
    float f = 0.0f;
    REQUIRE(read_quantized(stream, f, 0.0f, 100.0f, 10));
    REQUIRE(std::abs(f - 50.0f) < 0.2f);
    REQUIRE(read_quantized(stream, f, -100.0f, 100.0f, 12));
    REQUIRE(std::abs(f - 0.0f) < 0.1f);
    REQUIRE(read_quantized(stream, f, -100.0f, 100.0f, 8));
    REQUIRE(std::abs(f - (-50.0f)) < 1.0f);
}

TEST_CASE("Smallest-three quaternion roundtrip", "[serialization]") {
    BitStream stream;
    std::array<float, 4> quat = {0.0f, 0.0f, 0.0f, 1.0f}; // identity
    write_smallest_three(stream, quat, 16);

    stream.reset_read();
    std::array<float, 4> decoded{};
    REQUIRE(read_smallest_three(stream, decoded, 16));

    // Normalize and compare
    float len = std::sqrt(decoded[0] * decoded[0] + decoded[1] * decoded[1] + decoded[2] * decoded[2]
                          + decoded[3] * decoded[3]);
    REQUIRE(len > 0.0f);
    for (std::size_t i = 0; i < 4; ++i) decoded[i] /= len;

    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(std::abs(decoded[i] - quat[i]) < 0.02f);
    }
}

TEST_CASE("Member-serializable struct roundtrip", "[serialization]") {
    struct PlayerState {
        uint32_t id = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool alive = false;

        void serialize(BitStream& stream) const {
            systems::leal::campello_net::serialization::serialize(stream, id);
            systems::leal::campello_net::serialization::serialize(stream, x);
            systems::leal::campello_net::serialization::serialize(stream, y);
            systems::leal::campello_net::serialization::serialize(stream, z);
            systems::leal::campello_net::serialization::serialize(stream, alive);
        }

        bool deserialize(BitStream& stream) {
            return systems::leal::campello_net::serialization::deserialize(stream, id)
                && systems::leal::campello_net::serialization::deserialize(stream, x)
                && systems::leal::campello_net::serialization::deserialize(stream, y)
                && systems::leal::campello_net::serialization::deserialize(stream, z)
                && systems::leal::campello_net::serialization::deserialize(stream, alive);
        }
    };

    BitStream stream;
    PlayerState original{42, 10.5f, 20.0f, -5.0f, true};
    systems::leal::campello_net::serialization::serialize(stream, original);

    stream.reset_read();
    PlayerState decoded;
    REQUIRE(systems::leal::campello_net::serialization::deserialize(stream, decoded));
    REQUIRE(decoded.id == 42);
    REQUIRE(decoded.x == 10.5f);
    REQUIRE(decoded.y == 20.0f);
    REQUIRE(decoded.z == -5.0f);
    REQUIRE(decoded.alive == true);
}

TEST_CASE("Serialization size benchmark", "[serialization][benchmark]") {
    constexpr int N = 1000;

    // Naive memcpy baseline
    std::vector<float> data(N);
    for (std::size_t i = 0; i < N; ++i) data[i] = static_cast<float>(i) * 0.1f;

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<uint8_t> baseline(N * sizeof(float));
    std::memcpy(baseline.data(), data.data(), N * sizeof(float));
    auto t1 = std::chrono::high_resolution_clock::now();

    // Half-precision serialization
    BitStream stream;
    auto t2 = std::chrono::high_resolution_clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        stream.write_half(data[i]);
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    (void)baseline;
    (void)(t1 - t0);
    (void)(t3 - t2);

    std::size_t base_bytes = N * sizeof(float);
    std::size_t half_bytes = stream.byte_count();

    REQUIRE(half_bytes == N * 2); // 16-bit per float
    REQUIRE(half_bytes < base_bytes); // 50% size reduction

    // Verify roundtrip
    stream.reset_read();
    for (int i = 0; i < N; ++i) {
        float f = 0.0f;
        REQUIRE(stream.read_half(f));
    }
}

#include "campello_net/network_clock.hpp"
#include "campello_net/network_time.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace systems::leal::campello_net;

static bool near_eq(float a, float b, float eps = 0.0002f) {
    return a > b - eps && a < b + eps;
}

static bool near_eq_d(double a, double b, double eps = 0.0002) {
    return a > b - eps && a < b + eps;
}

TEST_CASE("NetworkClock advances tick at correct rate") {
    NetworkClock clock(10.0f); // 10 Hz = 100 ms per tick

    REQUIRE(clock.tick() == 0);
    REQUIRE(clock.interpolation_factor() == 0.0f);

    clock.advance(0.05f); // half a tick
    REQUIRE(clock.tick() == 0);
    REQUIRE(near_eq(clock.interpolation_factor(), 0.5f));

    clock.advance(0.05f); // completes first tick
    REQUIRE(clock.tick() == 1);
    REQUIRE(near_eq(clock.interpolation_factor(), 0.0f));

    clock.advance(0.3f); // 3 more ticks
    REQUIRE(clock.tick() == 4);
}

TEST_CASE("NetworkClock interpolation_factor clamps to valid range") {
    NetworkClock clock(10.0f);

    clock.advance(0.15f); // 1.5 ticks
    REQUIRE(clock.tick() == 1);
    REQUIRE(near_eq(clock.interpolation_factor(), 0.5f));
}

TEST_CASE("NetworkClock time_until_next_tick") {
    NetworkClock clock(10.0f);

    clock.advance(0.03f);
    REQUIRE(near_eq(clock.time_until_next_tick(), 0.07f));

    clock.advance(0.07f);
    REQUIRE(clock.tick() == 1);
    REQUIRE(near_eq(clock.time_until_next_tick(), 0.1f));
}

TEST_CASE("NetworkClock reset clears state") {
    NetworkClock clock(10.0f);

    clock.advance(0.25f);
    REQUIRE(clock.tick() == 2);

    clock.reset();
    REQUIRE(clock.tick() == 0);
    REQUIRE(clock.interpolation_factor() == 0.0f);
}

TEST_CASE("NetworkClock align_to_server_tick snaps on large drift") {
    NetworkClock clock(10.0f);
    NetworkTime net_time;

    // Record a sample: RTT = 100 ms, offset = 0 ms (clocks aligned)
    // t0=0, t1=0.05, t2=0.05, t3=0.10
    net_time.record_sample(0.0, 0.05, 0.05, 0.10);
    clock.set_network_time(&net_time);

    // Advance local clock to tick 5
    clock.advance(0.5f);
    REQUIRE(clock.tick() == 5);

    // Server sent snapshot at tick 20 (server time = 2.0s).
    // One-way latency = 0.05s, so local arrival time ≈ 2.05s.
    // Expected server tick now ≈ 20 + 1 = 21.
    clock.align_to_server_tick(20, 2.05);

    // Large drift: |21 - 5| = 16 > 2, so snap
    REQUIRE(clock.tick() == 21);
    REQUIRE(clock.interpolation_factor() == 0.0f);
}

TEST_CASE("NetworkClock align_to_server_tick does not snap within threshold") {
    NetworkClock clock(10.0f);
    NetworkTime net_time;

    // RTT = 100 ms, offset = 0 ms
    net_time.record_sample(0.0, 0.05, 0.05, 0.10);
    clock.set_network_time(&net_time);

    // Advance to tick 20
    clock.advance(2.0f);
    REQUIRE(clock.tick() == 20);

    // Server sent snapshot at tick 19 (server time = 1.9s).
    // Local arrival ≈ 1.9 + 0.05 = 1.95s.
    // Expected = 19 + 1 = 20. Drift = 0.
    clock.align_to_server_tick(19, 1.95);
    REQUIRE(clock.tick() == 20);
}

TEST_CASE("NetworkClock align_to_server_tick accelerates when slightly behind") {
    NetworkClock clock(10.0f);
    NetworkTime net_time;

    // RTT = 100 ms, offset = 0 ms
    net_time.record_sample(0.0, 0.05, 0.05, 0.10);
    clock.set_network_time(&net_time);

    // Local tick = 20
    clock.advance(2.0f);
    REQUIRE(clock.tick() == 20);

    // Server sent snapshot at tick 20 (server time = 2.0s).
    // Local arrival ≈ 2.0 + 0.05 = 2.05s.
    // Expected = 20 + 1 = 21. Drift = +1, so accelerate.
    clock.align_to_server_tick(20, 2.05);
    // Acceleration bumps accumulator by 0.5 * tick_interval but doesn't immediately tick.
    REQUIRE(clock.tick() == 20);
    REQUIRE(near_eq(clock.interpolation_factor(), 0.5f));

    // Next advance only needs half a tick to trigger.
    clock.advance(0.05f);
    REQUIRE(clock.tick() == 21);
}

TEST_CASE("NetworkClock align_to_server_tick decelerates when slightly ahead") {
    NetworkClock clock(10.0f);
    NetworkTime net_time;

    // RTT = 100 ms, offset = 0 ms
    net_time.record_sample(0.0, 0.05, 0.05, 0.10);
    clock.set_network_time(&net_time);

    // Local tick = 22
    clock.advance(2.2f);
    REQUIRE(clock.tick() == 22);

    // Server sent snapshot at tick 20 (server time = 2.0s).
    // Local arrival ≈ 2.0 + 0.05 = 2.05s.
    // Expected = 20 + 1 = 21. Drift = -1, so decelerate.
    clock.align_to_server_tick(20, 2.05);
    // Deceleration reduces accumulator but never rewinds tick counter.
    REQUIRE(clock.tick() == 22);
    REQUIRE(near_eq(clock.time_until_next_tick(), 0.15f));
}

TEST_CASE("NetworkClock estimated_server_tick uses NetworkTime offset") {
    NetworkClock clock(10.0f);
    NetworkTime net_time;

    // RTT = 100 ms, offset = 50 ms (server is 50 ms ahead)
    net_time.record_sample(0.0, 0.05, 0.05, 0.10);
    clock.set_network_time(&net_time);

    // At local_time = 1.0, server_time = 1.05
    // Server tick = 1.05 / 0.1 = 10.5 → truncated to 10
    NetTick est = clock.estimated_server_tick(1.0);
    REQUIRE(est == 10);
}

TEST_CASE("NetworkClock estimated_server_tick falls back to local tick") {
    NetworkClock clock(10.0f);

    clock.advance(1.0f);
    REQUIRE(clock.tick() == 10);

    // No NetworkTime attached
    NetTick est = clock.estimated_server_tick(1.0);
    REQUIRE(est == 10);
}

TEST_CASE("NetworkClock align without NetworkTime snaps to server tick") {
    NetworkClock clock(10.0f);

    clock.advance(1.0f);
    REQUIRE(clock.tick() == 10);

    // No NetworkTime attached → direct snap
    clock.align_to_server_tick(42, 1.0);
    REQUIRE(clock.tick() == 42);
    REQUIRE(clock.interpolation_factor() == 0.0f);
}

TEST_CASE("NetworkTime EMA smoothing") {
    NetworkTime net_time;

    // First sample: offset = 15 ms, RTT = 130 ms
    // t0=0, t1=0.08, t2=0.10, t3=0.15
    // offset = ((0.08-0) + (0.10-0.15)) * 0.5 = (0.08 - 0.05) * 0.5 = 0.015
    // rtt    = (0.15-0) - (0.10-0.08) = 0.15 - 0.02 = 0.13
    net_time.record_sample(0.0, 0.08, 0.10, 0.15);
    REQUIRE(near_eq_d(net_time.offset(), 0.015));
    REQUIRE(near_eq_d(net_time.rtt(), 0.13));

    // Second sample: offset = 20 ms, RTT = 160 ms
    // t0=0, t1=0.10, t2=0.12, t3=0.18
    // offset = ((0.10-0) + (0.12-0.18)) * 0.5 = (0.10 - 0.06) * 0.5 = 0.02
    // rtt    = (0.18-0) - (0.12-0.10) = 0.18 - 0.02 = 0.16
    // EMA: offset = 0.015 + 0.1*(0.02 - 0.015) = 0.0155
    // EMA: rtt   = 0.13 + 0.2*(0.16 - 0.13) = 0.136
    net_time.record_sample(0.0, 0.10, 0.12, 0.18);
    REQUIRE(near_eq_d(net_time.offset(), 0.0155));
    REQUIRE(near_eq_d(net_time.rtt(), 0.136));
}

TEST_CASE("NetworkTime local_to_remote and remote_to_local") {
    NetworkTime net_time;
    // offset = 15 ms
    net_time.record_sample(0.0, 0.08, 0.10, 0.15);

    REQUIRE(near_eq_d(net_time.local_to_remote(1.0), 1.015));
    REQUIRE(near_eq_d(net_time.remote_to_local(1.015), 1.0));
}

TEST_CASE("NetworkClock and NetworkTime reset clears state") {
    NetworkTime net_time;
    net_time.record_sample(0.0, 0.01, 0.01, 0.02);
    REQUIRE(net_time.sample_count() == 1);

    net_time.reset();
    REQUIRE(net_time.sample_count() == 0);
    REQUIRE(net_time.offset() == 0.0);
    REQUIRE(net_time.rtt() == 0.0);
}

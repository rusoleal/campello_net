#include "campello_net/rate_limiter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace systems::leal::campello_net;

TEST_CASE("RateLimiter allows messages within rate", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_messages(10.0f, 5.0f); // 10/sec, burst 5

    // First 5 should pass (burst capacity)
    for (int i = 0; i < 5; ++i) {
        REQUIRE(rl.allow_message(100));
    }

    // Sixth should fail (bucket empty)
    REQUIRE(!rl.allow_message(100));
}

TEST_CASE("RateLimiter refills over time", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_messages(10.0f, 1.0f); // 10/sec, burst 1

    REQUIRE(rl.allow_message(100));
    REQUIRE(!rl.allow_message(100));

    // Wait for one token to refill
    std::this_thread::sleep_for(std::chrono::milliseconds(110));

    REQUIRE(rl.allow_message(100));
    REQUIRE(!rl.allow_message(100));
}

TEST_CASE("RateLimiter burst allows short spikes", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_messages(1.0f, 10.0f); // 1/sec, burst 10

    // Should allow 10 immediately
    for (int i = 0; i < 10; ++i) {
        REQUIRE(rl.allow_message(1));
    }
    REQUIRE(!rl.allow_message(1));
}

TEST_CASE("RateLimiter tracks byte limits independently", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_messages(100.0f, 100.0f); // generous message limit
    rl.configure_bytes(100.0f, 50.0f);     // 100 bytes/sec, burst 50

    // First 50 bytes should pass (consumes entire burst)
    REQUIRE(rl.allow_message(50));

    // Any additional bytes should fail immediately (bucket empty)
    REQUIRE(!rl.allow_message(1));
    REQUIRE(!rl.allow_message(10));
    REQUIRE(!rl.allow_message(51));

    // After refill, more bytes should pass
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    REQUIRE(rl.allow_message(10)); // ~12 tokens refilled
}

TEST_CASE("RateLimiter RPC limit is separate", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_messages(100.0f, 100.0f);
    rl.configure_rpcs(2.0f, 2.0f);

    REQUIRE(rl.allow_rpc());
    REQUIRE(rl.allow_rpc());
    REQUIRE(!rl.allow_rpc());

    // Messages are not affected by RPC limit
    REQUIRE(rl.allow_message(100));
    REQUIRE(rl.allow_message(100));
}

TEST_CASE("RateLimiter stats track allows and drops", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_messages(1.0f, 1.0f);

    REQUIRE(rl.allow_message(10));
    REQUIRE(!rl.allow_message(10));
    REQUIRE(!rl.allow_message(10));

    auto stats = rl.stats();
    REQUIRE(stats.messages_allowed == 1);
    REQUIRE(stats.messages_dropped == 2);
    REQUIRE(stats.bytes_allowed == 10);
    REQUIRE(stats.bytes_dropped == 20);
}

TEST_CASE("RateLimiter reset_stats clears counters", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_messages(1.0f, 1.0f);
    rl.allow_message(1);
    rl.allow_message(1);

    rl.reset_stats();
    auto stats = rl.stats();
    REQUIRE(stats.messages_allowed == 0);
    REQUIRE(stats.messages_dropped == 0);
}

TEST_CASE("RateLimiter disabled when rate is zero", "[rate_limiter]") {
    RateLimiter rl;
    // Default rate is 0, which means unlimited
    for (int i = 0; i < 100; ++i) {
        REQUIRE(rl.allow_message(1000));
    }
    REQUIRE(rl.allow_rpc());
}

TEST_CASE("RateLimiter byte burst recovers over time", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_bytes(100.0f, 50.0f); // 100 bytes/sec, burst 50

    REQUIRE(rl.allow_message(50));
    REQUIRE(!rl.allow_message(1)); // bucket empty

    // Wait half a second → ~50 tokens refill
    std::this_thread::sleep_for(std::chrono::milliseconds(510));

    REQUIRE(rl.allow_message(49));  // slightly under to account for timing jitter
    REQUIRE(!rl.allow_message(10)); // not quite enough left
}

TEST_CASE("RateLimiter bytes refill gradually", "[rate_limiter]") {
    RateLimiter rl;
    rl.configure_bytes(100.0f, 50.0f); // 100 bytes/sec, burst 50

    REQUIRE(rl.allow_message(50));
    REQUIRE(!rl.allow_message(1));

    // Wait 100ms → ~10 tokens refill
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    REQUIRE(rl.allow_message(8)); // conservative: should have ~12 tokens
}

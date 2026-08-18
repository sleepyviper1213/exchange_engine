#include "core/concurrency/lockfree/fast_queue.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>

using namespace exchange::core::concurrency::lockfree;

namespace {
/// @brief Push a string's bytes as one message; returns try_push's result.
bool push_str(FastQueue &q, std::string_view s) {
    return q.try_push(std::as_bytes(std::span(s.data(), s.size())));
}

/// @brief Pop one message into a string, or std::nullopt if the lockfree is empty.
std::optional<std::string> pop_str(FastQueue &q) {
    std::array<std::byte, 256> buf{};
    const auto n = q.try_pop(buf);
    if (!n) return std::nullopt;
    return std::string(reinterpret_cast<const char *>(buf.data()), *n);
}

} // namespace

// --------------------------------------------------------------------------
// Single-threaded behaviour
// --------------------------------------------------------------------------

TEST(FastQueue, CapacityIsRoundedUpToPowerOfTwo) {
    EXPECT_EQ(FastQueue(1000).capacity(), 1024u);
    EXPECT_EQ(FastQueue(1024).capacity(), 1024u);
    EXPECT_EQ(FastQueue(1025).capacity(), 2048u);
}

TEST(FastQueue, PopOnEmptyReturnsNullopt) {
    FastQueue q(64);
    EXPECT_TRUE(q.empty());
    std::array<std::byte, 16> buf{};
    EXPECT_FALSE(q.try_pop(buf).has_value());
}

TEST(FastQueue, PushThenPopRoundTrips) {
    FastQueue q(64);
    ASSERT_TRUE(push_str(q, "hello"));
    EXPECT_FALSE(q.empty());

    const auto out = pop_str(q);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "hello");
    EXPECT_TRUE(q.empty());
}

TEST(FastQueue, PreservesFifoOrderAcrossMessages) {
    FastQueue q(128);
    ASSERT_TRUE(push_str(q, "one"));
    ASSERT_TRUE(push_str(q, "two"));
    ASSERT_TRUE(push_str(q, "three"));

    EXPECT_EQ(pop_str(q).value_or(""), "one");
    EXPECT_EQ(pop_str(q).value_or(""), "two");
    EXPECT_EQ(pop_str(q).value_or(""), "three");
    EXPECT_FALSE(pop_str(q).has_value());
}

TEST(FastQueue, EmptyPayloadIsADistinctMessage) {
    FastQueue q(64);
    ASSERT_TRUE(push_str(q, ""));
    EXPECT_FALSE(q.empty());
    const auto out = pop_str(q);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "");
    EXPECT_TRUE(q.empty());
}

// --------------------------------------------------------------------------
// Unhappy paths / boundary conditions
// --------------------------------------------------------------------------

TEST(FastQueue, PushFailsWhenInsufficientRoom) {
    FastQueue q(16); // 16 bytes total; 4-byte header + payload per message
    ASSERT_TRUE(push_str(q, "abcdef"));   // needs 10 bytes
    EXPECT_FALSE(push_str(q, "abcdef"));  // 10 more won't fit in the remaining 6

    // Draining frees the space again.
    EXPECT_EQ(pop_str(q).value_or(""), "abcdef");
    EXPECT_TRUE(push_str(q, "abcdef"));
}

TEST(FastQueue, MessageSpanningTheWrapBoundaryIsReassembled) {
    // Repeatedly fill and drain so the cursors advance well past capacity,
    // forcing payloads to straddle the ring's wrap point.
    FastQueue q(16);
    const std::string msg = "wraps!"; // 4 + 6 = 10 bytes per message
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(push_str(q, msg)) << "iteration " << i;
        EXPECT_EQ(pop_str(q).value_or(""), msg) << "iteration " << i;
    }
    EXPECT_TRUE(q.empty());
}

// --------------------------------------------------------------------------
// Concurrent single-producer / single-consumer
// --------------------------------------------------------------------------

TEST(FastQueue, SpscThreadedRoundTripDeliversEveryMessageInOrder) {
    constexpr int kCount = 100'000;
    FastQueue q(1u << 16);

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            const auto payload = std::as_bytes(std::span(&i, 1));
            while (!q.try_push(payload)) {
                std::this_thread::yield(); // lockfree full - let the consumer catch up
            }
        }
    });

    int received = 0;
    int last = -1;
    bool ordered = true;
    while (received < kCount) {
        std::array<std::byte, sizeof(int)> buf{};
        const auto n = q.try_pop(buf);
        if (!n) {
            std::this_thread::yield(); // lockfree empty - wait for the producer
            continue;
        }
        ASSERT_EQ(*n, sizeof(int));
        int value = 0;
        std::memcpy(&value, buf.data(), sizeof(int));
        if (value != last + 1) ordered = false;
        last = value;
        ++received;
    }
    producer.join();

    EXPECT_EQ(received, kCount);
    EXPECT_TRUE(ordered);
    EXPECT_EQ(last, kCount - 1);
}

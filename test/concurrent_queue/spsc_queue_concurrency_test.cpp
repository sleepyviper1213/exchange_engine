#include "lockfree/queue/spsc_queue.hpp"

#include <gtest/gtest.h>

using namespace core::lockfree;

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <thread>

// Concurrency layer for spsc_queue: one producer thread + one consumer thread
// (the SPSC contract forbids more of either). These tests exercise the
// memory-ordering path — the acquire/release fences and the cached cursors —
// which single-threaded tests cannot reach. They assert only what survives
// non-determinism (item conservation + FIFO monotonicity of a single ordered
// producer), and are the tests meant to run under the ThreadSanitizer config,
// where TSan catches races even on interleavings that happen not to fail here.
// Exact-value / edge-case correctness lives in spsc_queue_test.cpp.

namespace {
inline constexpr uint32_t kStream = 200'000U;
inline constexpr uint32_t kChunk  = 64U;
inline constexpr uint64_t kExpectedSum =
	static_cast<uint64_t>(kStream) * (kStream - 1U) / 2U;

/// @brief Consumer-side validator for a producer that emits 0,1,2,... in order.
/// @details Not thread-safe: only the single consumer thread calls @c accept;
/// the observers are read on the main thread after @c join.
struct fifo_checker {
	uint64_t expected  = 0;
	uint64_t sum       = 0;
	uint64_t first_bad = 0;
	bool in_order      = true;

	void accept(uint64_t value) noexcept {
		if (in_order && value != expected) {
			in_order  = false;
			first_bad = value;
		}
		sum += value;
		++expected;
	}
};

void expect_full_stream(const fifo_checker &c) {
	EXPECT_TRUE(c.in_order) << "FIFO violated at position " << c.expected
							<< "; got " << c.first_bad;
	EXPECT_EQ(c.expected, kStream);
	EXPECT_EQ(c.sum, kExpectedSum);
}

/// @brief Instance-counting element; @c alive is atomic because the producer
/// constructs and the consumer destroys concurrently.
struct counted {
	static inline std::atomic<int> alive{0};
	int value = 0;

	explicit counted(int v = 0) noexcept : value(v) { alive.fetch_add(1); }

	counted(counted &&o) noexcept : value(o.value) { alive.fetch_add(1); }

	counted &operator=(counted &&o) noexcept {
		value = o.value;
		return *this;
	}

	~counted() { alive.fetch_sub(1); }
};
} // namespace

// --------------------------------------------------------------------------
// One-by-one producer / one-by-one consumer
// --------------------------------------------------------------------------

TEST(SpscQueueConcurrency, OneByOneTransfersInOrder) {
	spsc_queue<uint32_t, 1024> q;
	fifo_checker checker;

	std::thread producer{[&] {
		for (uint32_t i = 0; i < kStream; ++i)
			while (!q.try_emplace(i)) {}
	}};
	std::thread consumer{[&] {
		for (uint32_t i = 0; i < kStream; ++i) {
			uint32_t v = 0;
			while (!q.try_dequeue(v)) {}
			checker.accept(v);
		}
	}};
	producer.join();
	consumer.join();

	expect_full_stream(checker);
}

// --------------------------------------------------------------------------
// Batched producer (try_emplace_range) / range consumer (try_dequeue_range):
// the fully-batched pipeline the benchmark harness measures.
// --------------------------------------------------------------------------

TEST(SpscQueueConcurrency, BatchPushRangePopTransfersInOrder) {
	spsc_queue<uint32_t, 1024> q;
	fifo_checker checker;

	std::thread producer{[&] {
		std::array<uint32_t, kChunk> chunk{};
		for (uint32_t sent = 0; sent < kStream;) {
			const uint32_t n = std::min<uint32_t>(kChunk, kStream - sent);
			for (uint32_t j = 0; j < n; ++j) chunk[j] = sent + j;
			const std::span<const uint32_t> range(chunk.data(), n);
			while (!q.try_emplace_range(range)) {}
			sent += n;
		}
	}};
	std::thread consumer{[&] {
		std::array<uint32_t, kChunk> buf{};
		for (uint32_t got = 0; got < kStream;) {
			size_t popped = 0;
			while ((popped = q.try_dequeue_range(buf)) == 0) {}
			for (size_t k = 0; k < popped; ++k) checker.accept(buf[k]);
			got += static_cast<uint32_t>(popped);
		}
	}};
	producer.join();
	consumer.join();

	expect_full_stream(checker);
}

// --------------------------------------------------------------------------
// In-place consumer (consume_up_to) draining a one-by-one producer
// --------------------------------------------------------------------------

TEST(SpscQueueConcurrency, ConsumeUpToDrainsInOrder) {
	spsc_queue<uint32_t, 1024> q;
	fifo_checker checker;

	std::thread producer{[&] {
		for (uint32_t i = 0; i < kStream; ++i)
			while (!q.try_emplace(i)) {}
	}};
	std::thread consumer{[&] {
		for (uint32_t got = 0; got < kStream;) {
			size_t n = 0;
			while ((n = q.consume_up_to(kChunk, [&](uint32_t &v) noexcept {
					   checker.accept(v);
				   })) == 0) {}
			got += static_cast<uint32_t>(n);
		}
	}};
	producer.join();
	consumer.join();

	expect_full_stream(checker);
}

// --------------------------------------------------------------------------
// Tiny capacity: forces both sides to spin on full/empty almost every step,
// maximising contention on the shared cursors and the cached-cursor fences.
// --------------------------------------------------------------------------

TEST(SpscQueueConcurrency, TinyCapacityStressesFullEmptyBranches) {
	spsc_queue<uint32_t, 2> q;
	fifo_checker checker;

	std::thread producer{[&] {
		for (uint32_t i = 0; i < kStream; ++i)
			while (!q.try_emplace(i)) {}
	}};
	std::thread consumer{[&] {
		for (uint32_t i = 0; i < kStream; ++i) {
			uint32_t v = 0;
			while (!q.try_dequeue(v)) {}
			checker.accept(v);
		}
	}};
	producer.join();
	consumer.join();

	expect_full_stream(checker);
}

// --------------------------------------------------------------------------
// Non-trivial element: the producer constructs and the consumer destroys on
// different threads, so the lifetime count must still balance to zero.
// --------------------------------------------------------------------------

TEST(SpscQueueConcurrency, NonPodLifetimeBalancesAcrossThreads) {
	constexpr uint32_t stream = 50000U;
	const int base            = counted::alive.load();
	{
		spsc_queue<counted, 256> q;
		fifo_checker checker;

		std::thread producer{[&] {
			for (uint32_t i = 0; i < stream; ++i)
				while (!q.try_emplace(static_cast<int>(i))) {}
		}};
		std::thread consumer{[&] {
			for (uint32_t i = 0; i < stream; ++i) {
				counted out{};
				while (!q.try_dequeue(out)) {}
				checker.accept(static_cast<uint64_t>(out.value));
			}
		}};
		producer.join();
		consumer.join();

		EXPECT_TRUE(checker.in_order);
		EXPECT_EQ(checker.expected, stream);
	}
	EXPECT_EQ(counted::alive.load(), base);
}

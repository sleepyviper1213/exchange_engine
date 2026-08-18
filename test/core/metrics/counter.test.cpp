#include "core/metrics/counter.hpp"

#include <gtest/gtest.h>

#include <thread>

using exchange::core::metrics::counter;

namespace {

TEST(MetricsCounter, AFreshCounterIsZero) {
	const counter c;
	EXPECT_EQ(c.load(), 0U);
}

TEST(MetricsCounter, AddIncreasesByDelta) {
	counter c;
	c.add(5);
	c.add(37);
	EXPECT_EQ(c.load(), 42U);
}

TEST(MetricsCounter, IncrementAddsOne) {
	counter c;
	c.increment();
	c.increment();
	c.increment();
	EXPECT_EQ(c.load(), 3U);
}

TEST(MetricsCounter, ResetGoesBackToZero) {
	counter c;
	c.add(100);
	c.reset();
	EXPECT_EQ(c.load(), 0U);
}

// The contract counter.hpp documents: one writer thread, any number of
// readers. This is the writer side of it - a dedicated thread does every
// bump, the main thread only reads after joining, so there is nothing here
// for TSan to catch except a torn or lost store, which a wrong value would
// reveal regardless.
TEST(MetricsCounter, EveryIncrementFromTheWriterThreadIsVisibleAfterJoin) {
	constexpr std::uint64_t ITERATIONS = 200'000;
	counter c;

	std::thread writer([&] {
		for (std::uint64_t i = 0; i < ITERATIONS; ++i) c.increment();
	});
	writer.join();

	EXPECT_EQ(c.load(), ITERATIONS);
}

} // namespace

#include "topology.fixture.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

TEST(core_allocatorTest, DistinctPhysicalSpreadsAcrossCores) {
	core_allocator alloc(two_by_two());
	const auto producer = alloc.reserve("producer");
	const auto consumer = alloc.reserve("consumer");

	ASSERT_TRUE(producer.has_value());
	ASSERT_TRUE(consumer.has_value());
	// Each lands on a different physical core's primary sibling: 0 and 2.
	EXPECT_EQ(*producer, 0U);
	EXPECT_EQ(*consumer, 2U);
	EXPECT_EQ(alloc.free_cores(), 2U);
}

TEST(core_allocatorTest, ReserveIsIdempotentPerRole) {
	core_allocator alloc(two_by_two());
	const auto first = alloc.reserve("engine");
	const auto again = alloc.reserve("engine");

	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first, again);
	EXPECT_EQ(first, alloc.core_for("engine"));
	// A second distinct call consumed no extra core.
	EXPECT_EQ(alloc.free_cores(), 3U);
}

TEST(core_allocatorTest, FallsBackToSiblingWhenPhysicalCoresExhausted) {
	// Single physical core with two SMT siblings.
	core_allocator alloc(make_topology({{0, 1}}));
	const auto a = alloc.reserve("a"); // takes the primary sibling
	const auto b = alloc.reserve("b"); // no fresh physical core -> sibling
	const auto c = alloc.reserve("c"); // nothing left

	ASSERT_TRUE(a.has_value());
	ASSERT_TRUE(b.has_value());
	EXPECT_EQ(*a, 0U);
	EXPECT_EQ(*b, 1U);
	EXPECT_NE(*a, *b);
	EXPECT_FALSE(c.has_value());
	EXPECT_EQ(alloc.free_cores(), 0U);
}

TEST(core_allocatorTest, NonDistinctReservationPacksLogicalCpus) {
	core_allocator alloc(two_by_two());
	const auto a =
		alloc.reserve("a", thread_priority::normal, /*distinct_physical=*/false);
	const auto b =
		alloc.reserve("b", thread_priority::normal, /*distinct_physical=*/false);

	ASSERT_TRUE(a.has_value());
	ASSERT_TRUE(b.has_value());
	// Ascending fill, sibling sharing allowed: cpus 0 then 1 (same core).
	EXPECT_EQ(*a, 0U);
	EXPECT_EQ(*b, 1U);
}

TEST(core_allocatorTest, UnknownRoleHasNoCore) {
	core_allocator alloc(two_by_two());
	EXPECT_FALSE(alloc.core_for("never-reserved").has_value());
	EXPECT_FALSE(alloc.pin_this_thread_to("never-reserved"));
}

} // namespace

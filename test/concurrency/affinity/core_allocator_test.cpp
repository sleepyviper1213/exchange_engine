#include "core/concurrency/affinity.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {
using namespace exchange::core::concurrency::affinity;

// Build a deterministic topology from explicit sibling groups so the tests do
// not depend on the host's real CPU layout. Group i is one physical core; the
// CoreIds it lists are that core's SMT siblings.
Topology make_topology(std::vector<std::vector<CoreId>> groups) {
	return detail::from_sibling_groups(
		std::move(groups));
}

// 2 physical cores, 2 SMT siblings each: cpus {0,1} and {2,3}.
Topology two_by_two() { return make_topology({{0, 1}, {2, 3}}); }

TEST(TopologyTest, SiblingGroupsBuildDenseModel) {
	const Topology t = two_by_two();
	EXPECT_EQ(t.logical_cpus, 4U);
	EXPECT_EQ(t.physical_cores, 2U);
	EXPECT_TRUE(t.smt);
	// Primaries are the lowest-id sibling of each physical core.
	EXPECT_EQ(t.primary_core_ids(), (std::vector<CoreId>{0U, 2U}));
}

TEST(CoreAllocatorTest, DistinctPhysicalSpreadsAcrossCores) {
	CoreAllocator alloc(two_by_two());
	const auto producer = alloc.reserve("producer");
	const auto consumer = alloc.reserve("consumer");

	ASSERT_TRUE(producer.has_value());
	ASSERT_TRUE(consumer.has_value());
	// Each lands on a different physical core's primary sibling: 0 and 2.
	EXPECT_EQ(*producer, 0U);
	EXPECT_EQ(*consumer, 2U);
	EXPECT_EQ(alloc.free_cores(), 2U);
}

TEST(CoreAllocatorTest, ReserveIsIdempotentPerRole) {
	CoreAllocator alloc(two_by_two());
	const auto first = alloc.reserve("engine");
	const auto again = alloc.reserve("engine");

	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first, again);
	EXPECT_EQ(first, alloc.core_for("engine"));
	// A second distinct call consumed no extra core.
	EXPECT_EQ(alloc.free_cores(), 3U);
}

TEST(CoreAllocatorTest, FallsBackToSiblingWhenPhysicalCoresExhausted) {
	// Single physical core with two SMT siblings.
	CoreAllocator alloc(make_topology({{0, 1}}));
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

TEST(CoreAllocatorTest, NonDistinctReservationPacksLogicalCpus) {
	CoreAllocator alloc(two_by_two());
	const auto a =
		alloc.reserve("a", ThreadPriority::Normal, /*distinct_physical=*/false);
	const auto b =
		alloc.reserve("b", ThreadPriority::Normal, /*distinct_physical=*/false);

	ASSERT_TRUE(a.has_value());
	ASSERT_TRUE(b.has_value());
	// Ascending fill, sibling sharing allowed: cpus 0 then 1 (same core).
	EXPECT_EQ(*a, 0U);
	EXPECT_EQ(*b, 1U);
}

TEST(CoreAllocatorTest, UnknownRoleHasNoCore) {
	CoreAllocator alloc(two_by_two());
	EXPECT_FALSE(alloc.core_for("never-reserved").has_value());
	EXPECT_FALSE(alloc.pin_this_thread_to("never-reserved"));
}

TEST(TopologyTest, DiscoverReturnsUsableLayout) {
	// The real host query must always yield a pinnable, self-consistent model.
	const Topology t = discover();
	EXPECT_GE(t.logical_cpus, 1U);
	EXPECT_GE(t.physical_cores, 1U);
	EXPECT_LE(t.physical_cores, t.logical_cpus);
	EXPECT_EQ(t.cores.size(), t.logical_cpus);
}

} // namespace

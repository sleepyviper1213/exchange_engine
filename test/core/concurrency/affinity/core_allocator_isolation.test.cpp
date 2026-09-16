// Placement when the kernel has isolated some CPUs. The ordering under test is
// the one core_allocator.hpp argues for: isolation beats the distinct-physical
// spread, because an isolated SMT sibling shares an L1 with one thread of ours
// and an unisolated core shares the machine with everything on it.

#include "topology.fixture.hpp"

#include <gtest/gtest.h>

namespace {

TEST(CoreAllocatorIsolation, PrefersAnIsolatedCoreOverAFreshPhysicalOne) {
	// cpus {0,1} and {2,3}; the kernel isolated the second physical core.
	// Without isolation the first reservation would take cpu 0.
	core_allocator alloc(make_isolated_topology(two_by_two(), {2, 3}));
	const auto matching = alloc.reserve("matching");

	ASSERT_TRUE(matching.has_value());
	EXPECT_EQ(*matching, 2U);
	EXPECT_TRUE(alloc.get_topology().is_isolated(*matching));
}

TEST(CoreAllocatorIsolation,
	 TakesAnIsolatedSiblingBeforeLeavingTheIsolatedSet) {
	// The isolated set is one physical core's two siblings, so the second role
	// chooses between cpu 3 (isolated, shares an L1 with the first role) and
	// cpu 0 (a whole physical core, shared with the rest of the system). It
	// takes 3: the interference that ruins a tail comes from the system, not
	// from the one thread we placed ourselves.
	core_allocator alloc(make_isolated_topology(two_by_two(), {2, 3}));
	const auto feed     = alloc.reserve("feed");
	const auto matching = alloc.reserve("matching");

	ASSERT_TRUE(feed.has_value());
	ASSERT_TRUE(matching.has_value());
	EXPECT_EQ(*feed, 2U);
	EXPECT_EQ(*matching, 3U);
}

TEST(CoreAllocatorIsolation, SpreadsAcrossPhysicalCoresWithinTheIsolatedSet) {
	// Four physical cores, two of them isolated whole: the distinct-physical
	// preference still applies, it just applies inside the isolated set first.
	// Order is 2 and 4 (primaries), then their siblings 3 and 5.
	core_allocator alloc(
		make_isolated_topology(make_topology({{0, 1}, {2, 3}, {4, 5}, {6, 7}}),
							   {2, 3, 4, 5}));

	EXPECT_EQ(alloc.reserve("a"), 2U);
	EXPECT_EQ(alloc.reserve("b"), 4U);
	EXPECT_EQ(alloc.reserve("c"), 3U);
	EXPECT_EQ(alloc.reserve("d"), 5U);
}

TEST(CoreAllocatorIsolation, FallsBackToHousekeepingCoresWhenTheSetRunsOut) {
	// The fifth role has no isolated CPU left. It is placed anyway - a role
	// that refused to run because the box is under-configured would be a worse
	// failure than one that runs with a worse tail - and core_allocator.cpp
	// logs a warning naming the CPU so the placement is not silent.
	core_allocator alloc(
		make_isolated_topology(make_topology({{0, 1}, {2, 3}, {4, 5}, {6, 7}}),
							   {2, 3, 4, 5}));
	for (const auto *role : {"a", "b", "c", "d"}) (void)alloc.reserve(role);

	const auto overflow = alloc.reserve("overflow");
	ASSERT_TRUE(overflow.has_value());
	EXPECT_EQ(*overflow, 0U); // first free primary sibling outside the set
	EXPECT_FALSE(alloc.get_topology().is_isolated(*overflow));
}

TEST(CoreAllocatorIsolation, PackingStillRespectsIsolationFirst) {
	// distinct_physical=false asks to pack logical CPUs; it does not ask to
	// give up isolation, so the ascending fill starts inside the isolated set.
	core_allocator alloc(make_isolated_topology(two_by_two(), {2, 3}));
	const auto a = alloc.reserve("a",
								 thread_priority::normal,
								 /*distinct_physical=*/false);
	const auto b = alloc.reserve("b",
								 thread_priority::normal,
								 /*distinct_physical=*/false);

	EXPECT_EQ(a, 2U);
	EXPECT_EQ(b, 3U);
}

TEST(CoreAllocatorIsolation, UnisolatedHostPlacesExactlyAsItDidBefore) {
	// The regression that matters most: on a dev box with no isolation the
	// preference collapses to the old distinct-physical walk.
	core_allocator alloc(two_by_two());

	EXPECT_EQ(alloc.reserve("producer"), 0U);
	EXPECT_EQ(alloc.reserve("consumer"), 2U);
}

TEST(CoreAllocatorIsolation, ReservationIsStillIdempotentPerRole) {
	core_allocator alloc(make_isolated_topology(two_by_two(), {2, 3}));
	const auto first = alloc.reserve("matching");
	const auto again = alloc.reserve("matching");

	EXPECT_EQ(first, again);
	EXPECT_EQ(alloc.free_cores(), 3U);
}

} // namespace

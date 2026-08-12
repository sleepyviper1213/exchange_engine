#include "topology.fixture.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

TEST(TopologyTest, SiblingGroupsBuildDenseModel) {
	const topology t = two_by_two();
	EXPECT_EQ(t.logical_cpus, 4U);
	EXPECT_EQ(t.physical_cores, 2U);
	EXPECT_TRUE(t.smt);
	// Primaries are the lowest-id sibling of each physical core.
	EXPECT_EQ(t.primary_core_ids(), (std::vector<core_id>{0U, 2U}));
}

TEST(TopologyTest, DiscoverReturnsUsableLayout) {
	// The real host query must always yield a pinnable, self-consistent model.
	const topology t = discover();
	EXPECT_GE(t.logical_cpus, 1U);
	EXPECT_GE(t.physical_cores, 1U);
	EXPECT_LE(t.physical_cores, t.logical_cpus);
	EXPECT_EQ(t.cores.size(), t.logical_cpus);
}

} // namespace

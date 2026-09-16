// Kernel isolation as it lands on a topology: which CPUs carry the flag, how
// many, and what happens to ids that name no CPU on this machine.

#include "topology.fixture.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

TEST(TopologyIsolation, FlagsOnlyTheCpusTheKernelIsolated) {
	const topology topo = make_isolated_topology(two_by_two(), {2, 3});

	EXPECT_EQ(topo.isolated_cpus, 2U);
	EXPECT_FALSE(topo.is_isolated(0));
	EXPECT_FALSE(topo.is_isolated(1));
	EXPECT_TRUE(topo.is_isolated(2));
	EXPECT_TRUE(topo.is_isolated(3));
	EXPECT_EQ(topo.isolated_core_ids(), std::vector<core_id>({2, 3}));
}

TEST(TopologyIsolation, AnUnisolatedHostFlagsNothing) {
	// Every dev box and CI runner. The absence has to be visible rather than
	// assumed away: a caller that cannot tell must not treat a core as quiet.
	const topology topo = two_by_two();

	EXPECT_EQ(topo.isolated_cpus, 0U);
	EXPECT_TRUE(topo.isolated_core_ids().empty());
	// `auto` rather than `core`: a unity batch also has namespace
	// exchange::core in scope, and the two are ambiguous. @see
	// test/CMakeLists.txt
	for (const auto &c : topo.cores) EXPECT_FALSE(c.isolated);
}

TEST(TopologyIsolation, IgnoresIdsThatNameNoCpuOnThisMachine) {
	// isolcpus=2-9 on a 4-CPU box. The kernel drops the surplus; so do we,
	// rather than growing the topology to match a boot line.
	const topology topo = make_isolated_topology(two_by_two(), {2, 3, 8, 9});

	EXPECT_EQ(topo.isolated_cpus, 2U);
	EXPECT_EQ(topo.isolated_core_ids(), std::vector<core_id>({2, 3}));
	EXPECT_FALSE(topo.is_isolated(9));
}

TEST(TopologyIsolation, AnUnknownCoreIsReportedUnisolated) {
	// The safe direction for a lookup miss: "I could not find this CPU" must
	// not read as "this CPU is undisturbed".
	const topology topo = make_isolated_topology(two_by_two(), {2, 3});

	EXPECT_FALSE(topo.is_isolated(77));
	EXPECT_FALSE(topo.is_isolated(NO_CORE));
}

TEST(TopologyIsolation, IsolationAndTicklessnessAreRecordedSeparately) {
	// A CPU can be out of the scheduler's domains and still take its 1 kHz
	// timer interrupt; that is the half-configured box, and it has to be
	// readable from the topology rather than inferred from the other flag.
	topology topo = two_by_two();
	exchange::core::concurrency::affinity::detail::apply_isolation(
		topo,
		isolation{.isolated = {2, 3}, .nohz_full = {3}});

	EXPECT_TRUE(topo.cores[2].isolated);
	EXPECT_FALSE(topo.cores[2].nohz_full);
	EXPECT_TRUE(topo.cores[3].isolated);
	EXPECT_TRUE(topo.cores[3].nohz_full);
	// nohz_full alone does not count as isolation.
	EXPECT_EQ(topo.isolated_cpus, 2U);
}

TEST(TopologyIsolation, ReapplyingReplacesRatherThanAccumulates) {
	// The overlay is idempotent in the way assign_llc is: applied twice, the
	// second answer wins and the count does not drift.
	topology topo = make_isolated_topology(two_by_two(), {2, 3});
	exchange::core::concurrency::affinity::detail::apply_isolation(
		topo,
		isolation{.isolated = {1}, .nohz_full = {}});

	EXPECT_EQ(topo.isolated_cpus, 1U);
	EXPECT_EQ(topo.isolated_core_ids(), std::vector<core_id>({1}));
}

} // namespace

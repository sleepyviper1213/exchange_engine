// The two kernel-text parsers behind discover_isolation(). A test cannot boot a
// kernel, and the formats are where this goes wrong in practice: an isolcpus=
// with flag words in front of the list, a nohz_full that reads back "(null)",
// and a /proc/cmdline whose parameters are one substring of each other away
// from being confused.

#include "core/concurrency/affinity/isolation.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

using namespace exchange::core::concurrency::affinity;

namespace {

using isolation_cpus = std::vector<core_id>;

// Fully qualified, and so is every detail:: below. Every module in this tree
// has a detail namespace and a unity batch puts several of them in scope at
// once - an unqualified detail:: here was ambiguous across four candidates.
// @see test/CMakeLists.txt, and the same note in topology.fixture.hpp.
namespace affinity_detail = exchange::core::concurrency::affinity::detail;

isolation_cpus isolation_parse(std::string_view list) {
	return affinity_detail::parse_cpu_list(list);
}

TEST(IsolationParse, ReadsASingleCpu) {
	EXPECT_EQ(isolation_parse("3"), isolation_cpus({3}));
}

TEST(IsolationParse, ExpandsAnInclusiveRange) {
	// The kernel's ranges include both ends: "2-5" is four CPUs, not three.
	EXPECT_EQ(isolation_parse("2-5"), isolation_cpus({2, 3, 4, 5}));
}

TEST(IsolationParse, ReadsTheMixedListSysfsActuallyPrints) {
	EXPECT_EQ(isolation_parse("0-1,4,6-7"), isolation_cpus({0, 1, 4, 6, 7}));
}

TEST(IsolationParse, TreatsAnEmptyFileAsNoIsolation) {
	EXPECT_TRUE(isolation_parse("").empty());
	EXPECT_TRUE(isolation_parse("\n").empty());
}

TEST(IsolationParse, TreatsTheNullSentinelAsNoIsolation) {
	// What an unset nohz_full reads back as on the kernels that print the
	// pointer rather than an empty line. Parsed as a CPU list it is a flag
	// word and would vanish anyway; naming it here says the case was
	// considered rather than survived by accident.
	EXPECT_TRUE(isolation_parse("(null)").empty());
}

TEST(IsolationParse, StripsTheTrailingNewlineSysfsAppends) {
	EXPECT_EQ(isolation_parse("2-3\n"), isolation_cpus({2, 3}));
}

TEST(IsolationParse, SkipsTheFlagWordsIsolcpusAllows) {
	// isolcpus=domain,managed_irq,2-7 - the flags are not CPUs, and reading
	// them as any would hand a role a core the kernel never isolated.
	EXPECT_EQ(isolation_parse("domain,managed_irq,2-7"),
			  isolation_cpus({2, 3, 4, 5, 6, 7}));
}

TEST(IsolationParse, DropsAStridedRangeRatherThanOverClaimIt) {
	// "0-7:1/2" is every 1st CPU of each group of 2 - CPUs 0, 2, 4, 6. This
	// parser does not model the stride, and the conservative answer is none:
	// over-claiming would pin a role to a CPU the kernel still schedules on.
	EXPECT_TRUE(isolation_parse("0-7:1/2").empty());
	// A well-formed neighbour in the same list still survives.
	EXPECT_EQ(isolation_parse("0-7:1/2,9"), isolation_cpus({9}));
}

TEST(IsolationParse, DropsAReversedOrMalformedRange) {
	EXPECT_TRUE(isolation_parse("7-2").empty());
	EXPECT_TRUE(isolation_parse("a-b").empty());
	EXPECT_TRUE(isolation_parse("3x").empty());
	EXPECT_TRUE(isolation_parse("-").empty());
}

TEST(IsolationParse, SortsAndDeduplicatesWhateverOrderItIsGiven) {
	// Nothing promises sysfs order, and is_isolated binary-searches the result.
	EXPECT_EQ(isolation_parse("5,1,5,2-3,1"), isolation_cpus({1, 2, 3, 5}));
}

TEST(IsolationParse, RefusesAnIdAboveTheKernelsOwnCpuCeiling) {
	// CONFIG_NR_CPUS tops out at 8192, so 4294967295 is a corrupt list rather
	// than a machine - and expanding it one id at a time would be the bug.
	EXPECT_TRUE(isolation_parse("0-4294967295").empty());
	EXPECT_TRUE(isolation_parse("99999").empty());
	EXPECT_EQ(isolation_parse("8191"), isolation_cpus({8191}));
}

TEST(IsolationParse, DegenerateRangeIsOneCpu) {
	EXPECT_EQ(isolation_parse("5-5"), isolation_cpus({5}));
}

TEST(IsolationParse, CmdlineValueReadsTheParameterItWasAskedFor) {
	constexpr std::string_view CMDLINE =
		"BOOT_IMAGE=/vmlinuz root=/dev/sda1 isolcpus=2-7 nohz_full=2-7 ro";
	EXPECT_EQ(affinity_detail::cmdline_value(CMDLINE, "isolcpus"), "2-7");
	EXPECT_EQ(affinity_detail::cmdline_value(CMDLINE, "nohz_full"), "2-7");
	EXPECT_EQ(affinity_detail::cmdline_value(CMDLINE, "root"), "/dev/sda1");
}

TEST(IsolationParse, CmdlineValueMatchesWholeParametersOnly) {
	// The trap a bare find() walks into: "isolcpus" appears inside both of
	// these, and answering from either would isolate the wrong set.
	EXPECT_TRUE(
		affinity_detail::cmdline_value("vendor_isolcpus=1-3", "isolcpus")
			.empty());
	EXPECT_TRUE(affinity_detail::cmdline_value("isolcpus_extra=1-3", "isolcpus")
					.empty());
	// A real one after a decoy is still found.
	EXPECT_EQ(affinity_detail::cmdline_value("vendor_isolcpus=1 isolcpus=4-5",
											 "isolcpus"),
			  "4-5");
}

TEST(IsolationParse, CmdlineValueHandlesTheParameterAtEitherEnd) {
	EXPECT_EQ(affinity_detail::cmdline_value("isolcpus=1-2 ro", "isolcpus"),
			  "1-2");
	EXPECT_EQ(affinity_detail::cmdline_value("ro isolcpus=1-2", "isolcpus"),
			  "1-2");
	EXPECT_EQ(affinity_detail::cmdline_value("ro isolcpus=1-2\n", "isolcpus"),
			  "1-2");
}

TEST(IsolationParse, CmdlineValueYieldsEmptyWhenTheParameterIsAbsentOrBare) {
	EXPECT_TRUE(affinity_detail::cmdline_value("ro quiet", "isolcpus").empty());
	// Present as a flag with no value - "isolcpus" alone isolates nothing.
	EXPECT_TRUE(affinity_detail::cmdline_value("ro isolcpus quiet", "isolcpus")
					.empty());
	EXPECT_TRUE(
		affinity_detail::cmdline_value("ro isolcpus", "isolcpus").empty());
	EXPECT_TRUE(affinity_detail::cmdline_value("ro isolcpus=1-2", "").empty());
}

TEST(IsolationParse, MembershipQueriesFollowTheParsedSets) {
	const isolation iso{.isolated  = affinity_detail::parse_cpu_list("2-5"),
						.nohz_full = affinity_detail::parse_cpu_list("3-5")};
	EXPECT_TRUE(iso.is_isolated(2));
	EXPECT_TRUE(iso.is_isolated(5));
	EXPECT_FALSE(iso.is_isolated(1));
	EXPECT_FALSE(iso.is_isolated(6));
	// Isolated from the scheduler is not isolated from the tick: cpu 2 is in
	// one set and not the other, which is the configuration mistake the two
	// separate lists exist to make visible.
	EXPECT_FALSE(iso.is_nohz_full(2));
	EXPECT_TRUE(iso.is_nohz_full(3));
	EXPECT_FALSE(iso.is_empty());
	EXPECT_TRUE(isolation{}.is_empty());
}

} // namespace

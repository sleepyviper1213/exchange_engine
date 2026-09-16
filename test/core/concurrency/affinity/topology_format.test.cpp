
#include "core/concurrency/affinity/format.hpp"

#include "orders/types.hpp"
#include "market_data/binance/endpoints.hpp"
#include "market_data/format.hpp"
#include "core/scaled/fixed_point.hpp"
#include "format.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace aff     = exchange::core::concurrency::affinity;
namespace binance = exchange::market_data::binance;
namespace md      = exchange::market_data;

using exchange::side_t;
using exchange::core::util::formattable_enum;
using exchange::engine::price_level;
using exchange::engine::orders::order;
using exchange::engine::order_book;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;
using exchange::engine::trade;

// Formatters for the affinity module'\''s types.

namespace {

TEST(TopologyFormat, SummarisesLogicalPhysicalSmtAndLlc) {
	// Two physical cores, two SMT siblings each, one shared last-level cache.
	aff::topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo),
			  "topology[4 logical / 2 physical cores, SMT, 1 LLC]");
}

TEST(TopologyFormat, PluralisesTheCacheCount) {
	aff::topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {{0, 2}, {1, 3}});
	EXPECT_EQ(fmt::format("{}", topo),
			  "topology[4 logical / 2 physical cores, SMT, 2 LLCs]");
}

TEST(TopologyFormat, NamesTheAbsenceOfSmt) {
	aff::topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo),
			  "topology[2 logical / 2 physical cores, no SMT, 1 LLC]");
}

TEST(TopologyFormat, CoreNamesItsCpuPhysicalCoreCacheAndSiblingRole) {
	aff::topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	// cores is sorted by core_id, so [0] and [2] are the two of physical core 0.
	EXPECT_EQ(fmt::format("{}", topo.cores[0]),
			  "core[cpu=0 core=0 llc=0 primary]");
	EXPECT_EQ(fmt::format("{}", topo.cores[2]),
			  "core[cpu=2 core=0 llc=0 sibling]");
}

TEST(TopologyFormat, CoreVectorPrintsElementWiseThroughRanges) {
	// No formatter<vector<core>> is written by hand - fmt/ranges.h composes it
	// from formatter<core>, which is why core needs one at all.
	aff::topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo.cores),
			  "[core[cpu=0 core=0 llc=0 primary], "
			  "core[cpu=1 core=1 llc=0 primary]]");
}

TEST(TopologyFormat, NamesTheIsolatedCpuCountOnlyWhenThereIsOne) {
	aff::topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	// An unconfigured host prints exactly what it printed before the isolation
	// fields existed - there is nothing to say and a "0 isolated" clause would
	// be noise in every startup log on every dev box.
	EXPECT_EQ(fmt::format("{}", topo),
			  "topology[4 logical / 2 physical cores, SMT, 1 LLC]");

	aff::detail::apply_isolation(
		topo,
		aff::isolation{.isolated = {1, 3}, .nohz_full = {}});
	EXPECT_EQ(fmt::format("{}", topo),
			  "topology[4 logical / 2 physical cores, SMT, 1 LLC, 2 isolated]");
}

TEST(TopologyFormat, CoreMarksIsolationAndTicklessnessAsSuffixes) {
	aff::topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	aff::detail::apply_isolation(
		topo,
		aff::isolation{.isolated = {1, 3}, .nohz_full = {3}});
	EXPECT_EQ(fmt::format("{}", topo.cores[0]),
			  "core[cpu=0 core=0 llc=0 primary]");
	// Isolated but still taking its timer interrupt - the half-configured box.
	EXPECT_EQ(fmt::format("{}", topo.cores[1]),
			  "core[cpu=1 core=1 llc=0 primary isolated]");
	EXPECT_EQ(fmt::format("{}", topo.cores[3]),
			  "core[cpu=3 core=1 llc=0 sibling isolated nohz]");
}

TEST(TopologyFormat, IsolationSuffixesRespectWidthAndAlignment) {
	// nested_formatter writes through write_padded, so the whole record pads -
	// including the suffixes, which are appended inside the same lambda rather
	// than after it.
	aff::topology topo = aff::detail::from_sibling_groups({{0}});
	aff::detail::assign_llc(topo, {});
	aff::detail::apply_isolation(
		topo,
		aff::isolation{.isolated = {0}, .nohz_full = {}});
	// 41 characters of record in a field of 45.
	EXPECT_EQ(fmt::format("{:>45}", topo.cores[0]),
			  "    core[cpu=0 core=0 llc=0 primary isolated]");
}

} // namespace

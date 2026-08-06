
#include "core/concurrency/affinity/format.hpp"

#include "trading-engine/orders/types.hpp"
#include "market-data/binance/endpoints.hpp"
#include "market-data/format.hpp"
#include "market-data/parser/fixed_point.hpp"
#include "trading-engine/format.hpp"

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
using exchange::engine::Trade;

// Formatters for the affinity module'\''s types.

namespace {

TEST(TopologyFormat, SummarisesLogicalPhysicalSmtAndLlc) {
	// Two physical cores, two SMT siblings each, one shared last-level cache.
	aff::Topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo),
			  "Topology[4 logical / 2 physical cores, SMT, 1 LLC]");
}

TEST(TopologyFormat, PluralisesTheCacheCount) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {{0, 2}, {1, 3}});
	EXPECT_EQ(fmt::format("{}", topo),
			  "Topology[4 logical / 2 physical cores, SMT, 2 LLCs]");
}

TEST(TopologyFormat, NamesTheAbsenceOfSmt) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo),
			  "Topology[2 logical / 2 physical cores, no SMT, 1 LLC]");
}

TEST(TopologyFormat, CoreNamesItsCpuPhysicalCoreCacheAndSiblingRole) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0, 2}, {1, 3}});
	aff::detail::assign_llc(topo, {});
	// cores is sorted by CoreId, so [0] and [2] are the two of physical core 0.
	EXPECT_EQ(fmt::format("{}", topo.cores[0]),
			  "Core[cpu=0 core=0 llc=0 primary]");
	EXPECT_EQ(fmt::format("{}", topo.cores[2]),
			  "Core[cpu=2 core=0 llc=0 sibling]");
}

TEST(TopologyFormat, CoreVectorPrintsElementWiseThroughRanges) {
	// No formatter<vector<Core>> is written by hand — fmt/ranges.h composes it
	// from formatter<Core>, which is why Core needs one at all.
	aff::Topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	EXPECT_EQ(fmt::format("{}", topo.cores),
			  "[Core[cpu=0 core=0 llc=0 primary], "
			  "Core[cpu=1 core=1 llc=0 primary]]");
}

} // namespace

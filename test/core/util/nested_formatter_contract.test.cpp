
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

// fmt::nested_formatter — fill, align and width apply to the whole record.

namespace {

TEST(NestedFormatterContract, WidthPadsTheWholeRecord) {
	const Trade trade{1, 2, 100, 10};
	const std::string bare = fmt::format("{}", trade);
	ASSERT_EQ(bare.size(), 34u);
	EXPECT_EQ(fmt::format("{:>46}", trade), std::string(12, ' ') + bare);
	EXPECT_EQ(fmt::format("{:<46}", trade), bare + std::string(12, ' '));
}

TEST(NestedFormatterContract, HonoursACustomFillCharacter) {
	md::l2_book book;
	book.set_level(side_t::bid, 15000, 7);
	const auto &level = book.bid_levels().front();
	EXPECT_EQ(fmt::format("[{:*<14}]", level), "[@15000 x 7****]");
	EXPECT_EQ(fmt::format("[{:>14}]", level), "[    @15000 x 7]");
}

TEST(NestedFormatterContract, WidthNarrowerThanTheRecordDoesNotTruncate) {
	const Trade trade{1, 2, 100, 10};
	EXPECT_EQ(fmt::format("{:>4}", trade), fmt::format("{}", trade));
}

TEST(NestedFormatterContract, AppliesToEveryRecordType) {
	aff::Topology topo = aff::detail::from_sibling_groups({{0}, {1}});
	aff::detail::assign_llc(topo, {});
	const order_book book;
	for (const std::string &padded :
		 {fmt::format("{:>60}", topo), fmt::format("{:>60}", book)})
		EXPECT_EQ(padded.size(), 60u);
}

} // namespace

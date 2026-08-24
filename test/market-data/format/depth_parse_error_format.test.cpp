
#include "core/concurrency/affinity/format.hpp"

#include "orders/types.hpp"
#include "market-data/binance/endpoints.hpp"
#include "market-data/format.hpp"
#include "market-data/parser/fixed_point.hpp"
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

// depth_parse_error - category, context and line, when each is present.

namespace {

TEST(DepthParseErrorFormat, CategoryAloneWhenThereIsNoContextOrLine) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   {},
										   0};
	EXPECT_EQ(fmt::format("{}", error), "invalid number");
}

TEST(DepthParseErrorFormat, ContextPrefixesTheCategory) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   "b[0]",
										   0};
	EXPECT_EQ(fmt::format("{}", error), "b[0]: invalid number");
}

TEST(DepthParseErrorFormat, LinePrefixesTheCategory) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   {},
										   7};
	EXPECT_EQ(fmt::format("{}", error), "line 7: invalid number");
}

TEST(DepthParseErrorFormat, LineThenContextThenCategory) {
	const binance::depth_parse_error error{binance::depth_error::bad_number,
										   "b[0]",
										   7};
	EXPECT_EQ(fmt::format("{}", error), "line 7: b[0]: invalid number");
}

} // namespace

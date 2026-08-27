// Formatters for the order vocabulary: the record every layer above passes
// around, in its three renderings.

#include "orders/format.hpp"
#include "orders/types.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

using exchange::side_t;
using exchange::engine::orders::order;
using exchange::engine::orders::order_type;
using exchange::engine::orders::time_in_force_instruction;

namespace {

TEST(OrdersFormat, OrderShowsIdSideSizeAndPolicy) {
	const order order{.id    = 7,
					  .side  = side_t::bid,
					  .tif   = time_in_force_instruction::IMMEDIATE_OR_CANCEL,
					  .price = 100,
					  .qty   = 10,
					  .timestamp = 0};
	EXPECT_EQ(fmt::format("{}", order),
			  "Order[id=7 bid 100 x 10 LIMIT IMMEDIATE_OR_CANCEL]");
	// "c" is the default spelled out, so it must render identically.
	EXPECT_EQ(fmt::format("{:c}", order), fmt::format("{}", order));
}

TEST(OrdersFormat, CompactOrderOmitsTheAbsentTriggerAndTimestamp) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	const auto text = fmt::format("{}", plain);
	EXPECT_EQ(text, "Order[id=7 bid 100 x 10 LIMIT GOOD_TILL_CANCELLED]");
	EXPECT_EQ(text.find("stop"), std::string::npos);
	EXPECT_EQ(text.find("ts="), std::string::npos);
}

TEST(OrdersFormat, CompactOrderShowsATriggerAndTimestampWhenSet) {
	const order stop{.id         = 7,
					 .side       = side_t::ask,
					 .type       = order_type::STOP,
					 .price      = 100,
					 .stop_price = 105,
					 .qty        = 10,
					 .timestamp  = 1234};
	EXPECT_EQ(
		fmt::format("{}", stop),
		"Order[id=7 ask 100 stop=105 x 10 STOP GOOD_TILL_CANCELLED ts=1234]");
}

TEST(OrdersFormat, VerboseOrderPrintsEveryFieldIncludingTheEmptyOnes) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	EXPECT_EQ(fmt::format("{:v}", plain),
			  "Order[id=7 side=bid price=100 stop_price=0 qty=10 type=LIMIT"
			  " tif=GOOD_TILL_CANCELLED timestamp=0]");
}

TEST(OrdersFormat, VerboseOrderKeepsItsShapeWhenFieldsAreSet) {
	const order stop{.id         = 7,
					 .side       = side_t::ask,
					 .type       = order_type::STOP,
					 .tif        = time_in_force_instruction::FILL_OR_KILL,
					 .price      = 100,
					 .stop_price = 105,
					 .qty        = 10,
					 .timestamp  = 1234};
	EXPECT_EQ(fmt::format("{:v}", stop),
			  "Order[id=7 side=ask price=100 stop_price=105 qty=10 type=STOP"
			  " tif=FILL_OR_KILL timestamp=1234]");
}

TEST(OrdersFormat, OrderModeComposesWithFillAlignAndWidth) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	const auto compact = fmt::format("{}", plain);
	const auto verbose = fmt::format("{:v}", plain);

	EXPECT_EQ(fmt::format("{:c >60}", plain),
			  std::string(60 - compact.size(), ' ') + compact);
	EXPECT_EQ(fmt::format("[{:v*<140}]", plain),
			  "[" + verbose + std::string(140 - verbose.size(), '*') + "]");
}

TEST(OrdersFormat, AModeLetterFollowedByAnAlignmentIsStillAFill) {
	const order plain{.id = 7, .side = side_t::bid, .price = 100, .qty = 10};
	const auto compact = fmt::format("{}", plain);

	// 'v' is the fill and '<' the alignment, so this stays compact, v-padded -
	// it does NOT select verbose.
	EXPECT_EQ(fmt::format("{:v<60}", plain),
			  compact + std::string(60 - compact.size(), 'v'));
	// Likewise 'c' here pads rather than selecting compact; the result is the
	// default rendering, which happens to be compact anyway.
	EXPECT_EQ(fmt::format("{:c>60}", plain),
			  std::string(60 - compact.size(), 'c') + compact);
}

} // namespace

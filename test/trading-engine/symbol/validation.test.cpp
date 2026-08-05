#include "symbol_spec.fixture.hpp"

#include "trading-engine/symbol.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange::test::symbol;

// validate — decimal text onto the engine grid, or the reason it cannot.

namespace {

TEST(Validation, WellFormedRequestBecomesAnOrderOnTheIntegerGrid) {
	const auto spec = equity();
	const order_request request{.id       = 7,
							   .symbol   = 1,
							   .side     = side_t::bid,
							   .price    = "49.99",
							   .quantity = "100"};

	const auto order = validate(request, spec);
	ASSERT_TRUE(order.has_value());
	EXPECT_EQ(order->id, 7U);
	EXPECT_EQ(order->side, side_t::bid);
	EXPECT_EQ(order->symbol_id, 1U); // carried through from the request
	EXPECT_EQ(order->price, 4999U);  // ticks, not cents
	EXPECT_EQ(order->qty, 100);      // lots
	EXPECT_EQ(order->tif, time_in_force_instruction::GOOD_TILL_CANCELLED);
}

TEST(Validation, EachFailureNamesItsOwnReason) {
	const auto spec = equity();
	const auto reject = [&](std::string_view price, std::string_view qty) {
		return validate({.id = 1, .symbol = 1, .side = side_t::bid,
						 .price = price, .quantity = qty},
						spec)
			.error();
	};

	EXPECT_EQ(reject("4x.99", "100"), reject_reason::MALFORMED_DECIMAL);
	EXPECT_EQ(reject("49.999", "100"), reject_reason::MALFORMED_DECIMAL);
	EXPECT_EQ(reject("100.00", "100"), reject_reason::PRICE_OUTSIDE_COLLAR);
	EXPECT_EQ(reject("49.99", "0"), reject_reason::NON_POSITIVE_QUANTITY);
	EXPECT_EQ(reject("49.99", "1.5"), reject_reason::MALFORMED_DECIMAL);
}

TEST(Validation, StopOrderConvertsBothPricesToTicks) {
	const auto spec = equity();
	const auto order = validate({.id         = 1,
								 .symbol     = 1,
								 .side       = side_t::ask,
								 .price      = "45.00",
								 .quantity   = "100",
								 .stop_price = "46.00",
								 .type       = order_type::STOP},
								spec);
	ASSERT_TRUE(order.has_value());
	EXPECT_EQ(order->price, 4500U);      // the limit it takes on once triggered
	EXPECT_EQ(order->stop_price, 4600U); // the level that triggers it
}

TEST(Validation, AStopOrderWithoutATriggerIsRefused) {
	const auto spec = equity();
	const auto err  = validate({.id       = 1,
								.symbol   = 1,
								.side     = side_t::ask,
								.price    = "45.00",
								.quantity = "100",
								.type     = order_type::STOP},
							   spec)
					     .error();
	EXPECT_EQ(err, reject_reason::MISSING_STOP_PRICE);
}

TEST(Validation, ATriggerOnANonStopOrderIsRefused) {
	const auto spec = equity();
	const auto err  = validate({.id         = 1,
								.symbol     = 1,
								.side       = side_t::bid,
								.price      = "49.99",
								.quantity   = "100",
								.stop_price = "48.00",
								.type       = order_type::LIMIT},
							   spec)
					     .error();
	EXPECT_EQ(err, reject_reason::UNEXPECTED_STOP_PRICE);
}

TEST(Validation, TheTriggerIsHeldToTheSameGridAndBandAsThePrice) {
	const symbol_spec nickel{3, "NICKEL", 2, 0, 5, 1, 10000, 1000}; // $100 ± 10%
	const auto reject = [&](std::string_view stop) {
		return validate({.id         = 1,
						 .symbol     = 3,
						 .side       = side_t::ask,
						 .price      = "100.00",
						 .quantity   = "1",
						 .stop_price = stop,
						 .type       = order_type::STOP},
						nickel)
			.error();
	};
	EXPECT_EQ(reject("100.03"), reject_reason::PRICE_NOT_ON_TICK);
	EXPECT_EQ(reject("500.00"), reject_reason::PRICE_OUTSIDE_COLLAR);
	EXPECT_EQ(reject("not a price"), reject_reason::MALFORMED_DECIMAL);
}

TEST(Validation, AnOrdinaryOrderLeavesTheTriggerAtZero) {
	const auto spec  = equity();
	const auto order = validate(
		{.id = 1, .symbol = 1, .side = side_t::bid, .price = "49.99", .quantity = "100"},
		spec);
	ASSERT_TRUE(order.has_value());
	EXPECT_EQ(order->stop_price, 0U); // the "not a stop" sentinel
}

TEST(Validation, TickErrorIsReportedBeforeTheCollar) {
	// A price both off the grid and outside the band reports the grid error,
	// because that is the one the client can act on.
	const symbol_spec nickel{3, "NICKEL", 2, 0, 5, 1, 10000, 1000}; // $100 ± 10%
	const auto err = validate({.id       = 1,
							   .symbol   = 3,
							   .side     = side_t::bid,
							   .price    = "500.03", // off-tick and far outside
							   .quantity = "1"},
							  nickel)
						 .error();
	EXPECT_EQ(err, reject_reason::PRICE_NOT_ON_TICK);
}

} // namespace

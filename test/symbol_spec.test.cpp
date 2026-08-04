#include "trading-engine/symbol.hpp"

#include <gtest/gtest.h>

#include <cstdint>

// The reject-don't-round boundary, ported from Emporia's
// ExchangeCoreExecutionVenueGateway.exactUnits:
//
//     value.divide(increment, 0, RoundingMode.UNNECESSARY).longValueExact();
//
// A price that is not an exact multiple of the tick throws there and is
// refused here. The tests below are mostly about the cases where a rounding
// implementation would have silently succeeded.

using namespace exchange;
using namespace exchange::engine;

namespace {

/// A US-equity-shaped listing: 2 decimals, penny tick, whole shares, $50
/// reference, ±20% band.
symbol_spec equity() {
	return symbol_spec{1, "ACME", 2, 0, 1, 1, 5000, 2000};
}

/// A crypto-shaped listing: 8 decimals on both sides, 0.01 price tick,
/// 0.00001 lot, $60,000 reference, ±20% band.
symbol_spec crypto() {
	return symbol_spec{2, "BTCUSDT", 8, 8, 1'000'000, 1'000, 6'000'000'000'000, 2000};
}

} // namespace

// --------------------------------------------------------------------------
// parse_exact_decimal — strict where market_data's parser is permissive
// --------------------------------------------------------------------------

TEST(ParseExactDecimal, ScalesWholeAndFractionalParts) {
	EXPECT_EQ(parse_exact_decimal("153.45", 2).value(), 15345);
	EXPECT_EQ(parse_exact_decimal("153", 2).value(), 15300);
	EXPECT_EQ(parse_exact_decimal("0.01", 2).value(), 1);
	EXPECT_EQ(parse_exact_decimal("+7.5", 3).value(), 7500); // short fraction pads
	EXPECT_EQ(parse_exact_decimal("42", 0).value(), 42);
}

// The difference that matters. market_data::parser::parse_fixed_point truncates
// the extra digit and returns 15345; a client that sent 153.456 would be filled
// at a price it did not ask for.
TEST(ParseExactDecimal, MoreFractionalDigitsThanTheScaleIsRefused) {
	EXPECT_FALSE(parse_exact_decimal("153.456", 2).has_value());
	EXPECT_EQ(parse_exact_decimal("153.456", 2).error(),
			  reject_reason::MALFORMED_DECIMAL);
	// Even when the extra digit is a zero: the listing cannot express it, so
	// accepting it would mean deciding on the client's behalf that it is safe.
	EXPECT_FALSE(parse_exact_decimal("153.450", 2).has_value());
}

TEST(ParseExactDecimal, MalformedInputIsRefused) {
	for (const auto *text : {"", "+", ".", "abc", "1.2.3", "1,5", "-1.00", " 1"})
		EXPECT_FALSE(parse_exact_decimal(text, 2).has_value()) << text;
}

TEST(ParseExactDecimal, OverflowIsRefusedRatherThanWrapped) {
	EXPECT_FALSE(parse_exact_decimal("99999999999999999999", 2).has_value());
	// Fits as digits but not once scaled.
	EXPECT_FALSE(parse_exact_decimal("9223372036854775807", 2).has_value());
}

// --------------------------------------------------------------------------
// Tick and lot alignment
// --------------------------------------------------------------------------

TEST(symbol_spec, PriceOnTheGridConvertsToTicks) {
	const auto spec = equity();
	EXPECT_EQ(spec.price_from_text("50.00").value(), 5000);
	EXPECT_EQ(spec.price_from_text("49.99").value(), 4999);
	// Round trip: every tick count has an exact decimal.
	EXPECT_EQ(spec.price_to_scaled(4999), 4999);
}

TEST(symbol_spec, PriceOffTheTickGridIsRefusedNotRounded) {
	// A nickel-tick listing: 153.45 is on the grid, 153.47 is not.
	const symbol_spec nickel{3, "NICKEL", 2, 0, 5, 1, 15345, 2000};
	EXPECT_EQ(nickel.price_from_text("153.45").value(), 3069);
	const auto off = nickel.price_from_text("153.47");
	ASSERT_FALSE(off.has_value());
	EXPECT_EQ(off.error(), reject_reason::PRICE_NOT_ON_TICK);
}

TEST(symbol_spec, QuantityOffTheLotGridIsRefused) {
	const auto spec = equity(); // whole shares
	EXPECT_EQ(spec.quantity_from_text("100").value(), 100);
	// qty_scale is 0, so a fractional share is not even representable text.
	EXPECT_EQ(spec.quantity_from_text("1.5").error(),
			  reject_reason::MALFORMED_DECIMAL);

	const auto btc = crypto(); // 0.00001 lot at scale 8
	EXPECT_EQ(btc.quantity_from_text("0.00001").value(), 1);
	EXPECT_EQ(btc.quantity_from_text("1.5").value(), 150000);
	EXPECT_EQ(btc.quantity_from_text("0.000001").error(),
			  reject_reason::QUANTITY_NOT_ON_LOT);
}

TEST(symbol_spec, NonPositiveValuesAreRefused) {
	const auto spec = equity();
	EXPECT_EQ(spec.price_from_scaled(0).error(), reject_reason::MALFORMED_DECIMAL);
	EXPECT_EQ(spec.quantity_from_scaled(0).error(),
			  reject_reason::NON_POSITIVE_QUANTITY);
}

// --------------------------------------------------------------------------
// The collar, and the array it sizes
// --------------------------------------------------------------------------

TEST(symbol_spec, CollarBandsAroundTheReferencePrice) {
	const auto spec = equity(); // $50.00 ± 20%, penny tick
	EXPECT_EQ(spec.collar_low(), 4000);  // $40.00
	EXPECT_EQ(spec.collar_high(), 6000); // $60.00
	EXPECT_TRUE(spec.within_collar(5000));
	EXPECT_TRUE(spec.within_collar(4000));
	EXPECT_TRUE(spec.within_collar(6000));
	EXPECT_FALSE(spec.within_collar(3999));
	EXPECT_FALSE(spec.within_collar(6001));
}

TEST(symbol_spec, CollarEdgesRoundInwardToTheTickGrid) {
	// $10.03 ± 20% is [8.024, 12.036], neither of which is on a penny tick.
	// Rounding inward keeps the band inside what the venue stated.
	const symbol_spec spec{4, "ODD", 2, 0, 1, 1, 1003, 2000};
	EXPECT_EQ(spec.collar_low(), 803);   // 8.024 -> 8.03, up
	EXPECT_EQ(spec.collar_high(), 1203); // 12.036 -> 12.03, down
}

TEST(symbol_spec, UncollaredListingsHaveNoBoundedPriceDomain) {
	const symbol_spec spec{5, "FREE", 2, 0, 1, 1, 5000, symbol_spec::NO_COLLAR};
	EXPECT_FALSE(spec.has_collar());
	EXPECT_TRUE(spec.within_collar(1));
	EXPECT_TRUE(spec.within_collar(1'000'000'000));
	// No band means no array length to derive — the caller must not try.
	EXPECT_EQ(spec.collar_span(), 0U);
}

TEST(symbol_spec, TickIndexMapsTheBandOntoZeroBasedSlots) {
	const auto spec = equity();
	EXPECT_EQ(spec.collar_span(), 2001U); // 4000..6000 inclusive
	EXPECT_EQ(spec.tick_index(spec.collar_low()), 0U);
	EXPECT_EQ(spec.tick_index(5000), 1000U);
	EXPECT_EQ(spec.tick_index(spec.collar_high()), spec.collar_span() - 1);
	EXPECT_EQ(spec.price_at_index(1000), 5000U);
}

// The question the collar exists to answer: is a price-indexed array viable
// for this listing? For an equity, comfortably. For a high-priced instrument
// on a fine tick, not at all — and the spec says so before anyone writes the
// allocator.
TEST(symbol_spec, CollarSpanDecidesWhetherAnIndexedBookIsAffordable) {
	constexpr std::size_t LEVEL_BYTES = 64; // sizeof(engine::price_Level)

	const auto eq = equity();
	EXPECT_EQ(eq.collar_span(), 2001U);
	EXPECT_LT(eq.indexed_side_bytes(LEVEL_BYTES), 128U * 1024U); // ~80 KB

	const auto btc = crypto(); // $60,000 ± 20%, $0.01 tick
	EXPECT_EQ(btc.collar_span(), 2'400'001U);
	EXPECT_GT(btc.indexed_side_bytes(LEVEL_BYTES), 90U * 1024U * 1024U); // ~92 MB
}

// --------------------------------------------------------------------------
// The validation stage
// --------------------------------------------------------------------------

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
	EXPECT_EQ(order->price, 4999U); // ticks, not cents
	EXPECT_EQ(order->qty, 100);     // lots
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

// --------------------------------------------------------------------------
// Stop orders carry a second price, and the two imply each other
// --------------------------------------------------------------------------

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

// The other direction matters just as much: a trigger on a limit order is a
// price the client meant something by, and ignoring it would be a guess.
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

TEST(symbol_registry, UnknownSymbolIsRejectedBeforeAnythingIsParsed) {
	symbol_registry registry;
	registry.add(equity());

	const auto err = registry
						 .validate({.id       = 1,
									.symbol   = 999,
									.side     = side_t::bid,
									.price    = "not even a number",
									.quantity = "also not"})
						 .error();
	EXPECT_EQ(err, reject_reason::UNKNOWN_SYMBOL);
}

TEST(symbol_registry, FindsRegisteredListingsAndReplacesOnReAdd) {
	symbol_registry registry;
	registry.add(equity());
	ASSERT_NE(registry.find(1), nullptr);
	EXPECT_EQ(registry.find(1)->symbol(), "ACME");
	EXPECT_EQ(registry.find(2), nullptr);

	// An operator correcting the tick size mid-session.
	registry.add(symbol_spec{1, "ACME", 2, 0, 5, 1, 5000, 2000});
	EXPECT_EQ(registry.find(1)->tick_scaled(), 5);
	EXPECT_EQ(registry.by_id.size(), 1U);
}

TEST(symbol_registry, ValidatesThroughTheRegisteredSpec) {
	symbol_registry registry;
	registry.add(equity());

	const auto order = registry.validate({.id       = 3,
										  .symbol   = 1,
										  .side     = side_t::ask,
										  .price    = "50.01",
										  .quantity = "25"});
	ASSERT_TRUE(order.has_value());
	EXPECT_EQ(order->price, 5001U);
	EXPECT_EQ(order->qty, 25);
}

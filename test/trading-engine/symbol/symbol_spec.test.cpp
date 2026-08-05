#include "symbol_spec.fixture.hpp"

#include "trading-engine/symbol.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange::test::symbol;

// symbol_spec — the tick/lot/collar grid a listing trades on.

namespace {

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

TEST(symbol_spec, CollarSpanDecidesWhetherAnIndexedBookIsAffordable) {
	constexpr std::size_t LEVEL_BYTES = 64; // sizeof(engine::price_Level)

	const auto eq = equity();
	EXPECT_EQ(eq.collar_span(), 2001U);
	EXPECT_LT(eq.indexed_side_bytes(LEVEL_BYTES), 128U * 1024U); // ~80 KB

	const auto btc = crypto(); // $60,000 ± 20%, $0.01 tick
	EXPECT_EQ(btc.collar_span(), 2'400'001U);
	EXPECT_GT(btc.indexed_side_bytes(LEVEL_BYTES), 90U * 1024U * 1024U); // ~92 MB
}

} // namespace

#include "symbol_spec.fixture.hpp"

#include "symbol.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::orders;

// symbol_spec - the tick/lot/collar grid a listing trades on.

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

// --------------------------------------------------------------------------
// The 64 -> 32 bit narrowing.
//
// A scaled decimal is 64-bit and a tick count is 32-bit, and this class holds
// the only division between them. Everything downstream - the book's price
// ordering, order_state's quantity field - assumes the result fits. These are
// the cases where it does not, and the refusal is what keeps a wrapped value
// from becoming a price the book would happily sort and match at.
// --------------------------------------------------------------------------

TEST(symbol_spec, PricePastTheTickDomainIsRefusedNotWrapped) {
	// A one-unit tick at scale 0, so scaled value == tick count and the bound
	// is reached by the input alone.
	const symbol_spec fine{7, "FINE", 0, 0, 1, 1, 1000};

	constexpr std::int64_t max_ticks =
		static_cast<std::int64_t>(std::numeric_limits<price_t>::max());
	EXPECT_EQ(fine.price_from_scaled(max_ticks).value(),
			  std::numeric_limits<price_t>::max());

	const auto over = fine.price_from_scaled(max_ticks + 1);
	ASSERT_FALSE(over.has_value());
	EXPECT_EQ(over.error(), reject_reason::PRICE_OUT_OF_RANGE);
}

TEST(symbol_spec, QuantityPastTheLotDomainIsRefusedNotWrapped) {
	const symbol_spec fine{8, "FINE", 0, 0, 1, 1, 1000};

	constexpr std::int64_t max_lots =
		static_cast<std::int64_t>(std::numeric_limits<quantity_t>::max());
	EXPECT_EQ(fine.quantity_from_scaled(max_lots).value(),
			  std::numeric_limits<quantity_t>::max());

	const auto over = fine.quantity_from_scaled(max_lots + 1);
	ASSERT_FALSE(over.has_value());
	EXPECT_EQ(over.error(), reject_reason::QUANTITY_OUT_OF_RANGE);
}

TEST(symbol_spec, ARealisticCryptoListingStaysWellInsideTheTickDomain) {
	// The case that motivated splitting the types: at scale 8 a five-figure
	// price is ~10^12 scaled, which does not fit price_t at all. Divided by a
	// realistic tick it is ~10^6, which fits with three orders of magnitude to
	// spare. The scaled value is market_data's problem; only the quotient is
	// ever the engine's.
	const symbol_spec btc{9, "BTCUSDT", 8, 8, 1'000'000, 1, 6'000'000'000'000};

	const auto ticks = btc.price_from_scaled(6'000'000'000'000);
	ASSERT_TRUE(ticks.has_value());
	EXPECT_EQ(*ticks, 6'000'000U);
	EXPECT_LT(*ticks, std::numeric_limits<price_t>::max() / 100);
}

TEST(symbol_spec, OffGridIsCheckedBeforeRange) {
	// An out-of-range price that is also off the tick grid reports the grid,
	// because that is the fault the client can actually act on.
	const symbol_spec nickel{10, "NICKEL", 0, 0, 5, 1, 1000};
	const auto both = nickel.price_from_scaled(
		(static_cast<std::int64_t>(std::numeric_limits<price_t>::max()) + 1) * 5 +
		1);
	ASSERT_FALSE(both.has_value());
	EXPECT_EQ(both.error(), reject_reason::PRICE_NOT_ON_TICK);
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
	// No band means no array length to derive - the caller must not try.
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
	constexpr std::size_t LEVEL_BYTES = 64; // sizeof(engine::price_level)

	const auto eq = equity();
	EXPECT_EQ(eq.collar_span(), 2001U);
	EXPECT_LT(eq.indexed_side_bytes(LEVEL_BYTES), 128U * 1024U); // ~80 KB

	const auto btc = crypto(); // $60,000 ± 20%, $0.01 tick
	EXPECT_EQ(btc.collar_span(), 2'400'001U);
	EXPECT_GT(btc.indexed_side_bytes(LEVEL_BYTES), 90U * 1024U * 1024U); // ~92 MB
}

} // namespace

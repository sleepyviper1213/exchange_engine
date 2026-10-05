#include "symbol_spec.fixture.hpp"

#include "symbol.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace exchange;
using namespace exchange::engine;

// symbol_spec::volume_from_scaled - the truncating crossing for aggregates,
// where quantity_from_scaled would refuse.

namespace {

TEST(SymbolSpecVolume, RoundsAPartialLotDown) {
	// A lot of 10 scaled units: 25 is two and a half lots, and the half is
	// evidence for less than a lot, so it is dropped rather than refused.
	const symbol_spec tens{11,
						   "TENS",
						   0,
						   0,
						   1 * units::scaled_price,
						   10 * units::scaled_size,
						   at_scaled(100)};
	EXPECT_EQ(tens.volume_from_scaled(25 * units::scaled_size),
			  2 * units::lot);
	EXPECT_EQ(tens.volume_from_scaled(9 * units::scaled_size), volume_t{});
	EXPECT_EQ(tens.volume_from_scaled(30 * units::scaled_size),
			  3 * units::lot);
	// Where the strict crossing refuses the same size outright.
	EXPECT_FALSE(tens.quantity_from_scaled(25 * units::scaled_size));
}

TEST(SymbolSpecVolume, NonPositiveIsZero) {
	const auto spec = equity();
	EXPECT_EQ(spec.volume_from_scaled(0 * units::scaled_size), volume_t{});
	EXPECT_EQ(spec.volume_from_scaled(-5 * units::scaled_size), volume_t{});
}

TEST(SymbolSpecVolume, AnAggregateMayExceedOneOrdersRange) {
	// A sum across prints is not an order, so it is not bounded by one.
	const auto spec = equity(); // lot of 1 at scale 0
	const std::int64_t past_one_order =
		std::int64_t{std::numeric_limits<std::int32_t>::max()} + 1;
	EXPECT_EQ(spec.volume_from_scaled(past_one_order * units::scaled_size),
			  past_one_order * units::lot);
	EXPECT_FALSE(
		spec.quantity_from_scaled(past_one_order * units::scaled_size));
}

} // namespace

#include "core/scaled/decimal.hpp"

#include <gtest/gtest.h>

// Scaled integer out to the decimal text a venue reads. Every case here is a
// price or a size that would be sent as an order, so a wrong answer is not a
// formatting bug - it is an order at a price nobody asked for.

using exchange::core::scaled::to_decimal;

TEST(ToDecimal, RendersAtExactlyTheRequestedScale) {
	EXPECT_EQ(to_decimal(15345, 2), "153.45");
	EXPECT_EQ(to_decimal(1, 8), "0.00000001");
}

TEST(ToDecimal, KeepsTrailingZerosRatherThanTrimming) {
	// The output depends on the scale alone, never on the value - which is what
	// makes it match the grid the venue published for the listing.
	EXPECT_EQ(to_decimal(100, 3), "0.100");
	EXPECT_EQ(to_decimal(15300, 2), "153.00");
}

TEST(ToDecimal, AZeroScaleRendersAnIntegerWithNoPoint) {
	EXPECT_EQ(to_decimal(42, 0), "42");
	// A negative scale is nonsense rather than an error worth a return type;
	// it is read as zero.
	EXPECT_EQ(to_decimal(42, -1), "42");
}

TEST(ToDecimal, PadsAFractionShorterThanTheScale) {
	// The trap: 5 at scale 3 is 0.005, not 0.5. Formatting the remainder
	// without zero-padding it is the classic way to send an order a thousand
	// times too large.
	EXPECT_EQ(to_decimal(5, 3), "0.005");
	EXPECT_EQ(to_decimal(50, 3), "0.050");
}

TEST(ToDecimal, ZeroRendersAsZeroAtTheScale) {
	EXPECT_EQ(to_decimal(0, 2), "0.00");
	EXPECT_EQ(to_decimal(0, 0), "0");
}

TEST(ToDecimal, ANegativeValueKeepsItsSignOutsideTheDigits) {
	// Integer division truncates towards zero, so the naive split renders
	// -0.05 as "0.-5". Nothing on the order path should be negative, but a
	// position or a PnL rendered through here would be.
	EXPECT_EQ(to_decimal(-5, 2), "-0.05");
	EXPECT_EQ(to_decimal(-15345, 2), "-153.45");
}

TEST(ToDecimal, HandlesTheExtremesOfTheScaledType) {
	EXPECT_EQ(to_decimal(9'223'372'036'854'775'807, 0), "9223372036854775807");
	// The one value whose negation overflows a signed 64-bit integer, which is
	// why the sign is taken off before the magnitude is computed.
	EXPECT_EQ(to_decimal(-9'223'372'036'854'775'807 - 1, 0),
			  "-9223372036854775808");
}

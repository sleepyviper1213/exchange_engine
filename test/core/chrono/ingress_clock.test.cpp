// The stamp a reaction time is measured from: that it is monotonic, that it is
// not confusable with venue time, and that "nobody stamped this" is a value.
//
// Small, and deliberately so: the type exists to make one mistake unspellable,
// and what is worth pinning is the property rather than the arithmetic.

#include "core/chrono/ingress.hpp"
#include "market_data/normalised.hpp" // depth_event, book_snapshot, timestamp

#include <gtest/gtest.h>

#include <chrono>
#include <type_traits>

using exchange::core::chrono::has_ingress;
using exchange::core::chrono::ingress_clock;
using exchange::core::chrono::ingress_time;
using exchange::market_data::book_snapshot;
using exchange::market_data::depth_event;
using exchange::market_data::timestamp;

TEST(IngressClock, DefaultStampReadsAsUnstamped) {
	// The whole reason the default is a sentinel: a replayed capture and a
	// scripted event both arrive without one, and treating the clock's epoch as
	// an arrival time would report the process's uptime as a reaction.
	EXPECT_FALSE(has_ingress(ingress_time{}));
	EXPECT_FALSE(has_ingress(depth_event{}.ingress));
	EXPECT_FALSE(has_ingress(book_snapshot{}.ingress));
}

TEST(IngressClock, ARealReadingIsStamped) {
	EXPECT_TRUE(has_ingress(ingress_clock::now()));
}

TEST(IngressClock, NeverGoesBackwards) {
	// What makes a difference between two readings an interval rather than a
	// guess. A steady clock is allowed to not advance between two adjacent
	// reads, so the assertion is >=, not >.
	const ingress_time first  = ingress_clock::now();
	const ingress_time second = ingress_clock::now();
	EXPECT_GE(second, first);
	EXPECT_GE((second - first).count(), 0);
	EXPECT_TRUE(ingress_clock::is_steady);
}

TEST(IngressClock, MeasuresInNanoseconds) {
	// A histogram of this is documented as nanoseconds, and the conversion
	// happens nowhere between here and record(), so the period is the contract.
	static_assert(std::is_same_v<ingress_clock::period,
								 std::chrono::nanoseconds::period>);
	static_assert(
		std::is_same_v<ingress_clock::duration,
					   std::chrono::duration<std::int64_t, std::nano>>);
	SUCCEED();
}

TEST(IngressClock, DoesNotMixWithVenueTime) {
	// The point of the type. Subtracting a venue event time from a local
	// reading measures the two machines' clock offset plus the one-way delay,
	// summed into a number nobody can decompose - so the two are made
	// unassignable rather than merely documented as different.
	static_assert(!std::is_convertible_v<timestamp, ingress_time>);
	static_assert(!std::is_convertible_v<ingress_time, timestamp>);
	static_assert(!std::is_assignable_v<ingress_time &, timestamp>);
	SUCCEED();
}

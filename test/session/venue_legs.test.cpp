#include "session/venue_legs.hpp"

#include <gtest/gtest.h>

// The chain of venue orders one engine order becomes when the venue can only
// reprice by cancel-replace.
//
// Every case is one of the three failures this exists to prevent, seen from
// the inside: a cancel naming a leg that is gone, a fill total that restarts
// at zero, and a leg that finished before anybody was told it existed.

using exchange::order_id_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::orders::time_in_force_instruction;
using exchange::session::replace_answer;
using exchange::session::venue_legs;
namespace units = exchange::units;

namespace {

constexpr order_id_t LEGS_ID = 7;

[[nodiscard]] venue_legs legs_with_one_order() {
	venue_legs legs;
	legs.open(LEGS_ID,
			  side_t::bid,
			  time_in_force_instruction::GOOD_TILL_CANCELLED);
	return legs;
}

[[nodiscard]] quantity_t legs_lots(std::int64_t n) { return n * units::lot; }

} // namespace

TEST(SessionVenueLegs, AFreshOrderIsWorkingAsLegZero) {
	const venue_legs legs = legs_with_one_order();
	EXPECT_EQ(legs.working_leg(LEGS_ID), 0U);
	EXPECT_EQ(legs.working_leg(LEGS_ID + 1), 0U) << "untracked reads as zero";
}

TEST(SessionVenueLegs, AReplaceMovesTheWorkingLegOnlyWhenTheVenueSaysSo) {
	venue_legs legs = legs_with_one_order();

	EXPECT_EQ(legs.begin_replace(LEGS_ID), 1U);
	// In flight: a cancel now still has to name the leg the venue has.
	EXPECT_EQ(legs.working_leg(LEGS_ID), 0U);

	legs.resolve(LEGS_ID, replace_answer::replaced);
	EXPECT_EQ(legs.working_leg(LEGS_ID), 1U);
}

TEST(SessionVenueLegs, AnUnchangedAnswerKeepsTheOldLegAndNeverReusesTheNumber) {
	venue_legs legs = legs_with_one_order();

	ASSERT_EQ(legs.begin_replace(LEGS_ID), 1U);
	legs.resolve(LEGS_ID, replace_answer::unchanged);
	EXPECT_EQ(legs.working_leg(LEGS_ID), 0U);

	// "Unchanged" may be an answer we could not read; if the replacement did
	// land, reusing its id would be refused while it is still working.
	EXPECT_EQ(legs.begin_replace(LEGS_ID), 2U);
}

TEST(SessionVenueLegs, TradedIsSummedAcrossLegs) {
	venue_legs legs = legs_with_one_order();
	ASSERT_TRUE(legs.on_report(LEGS_ID, 0, legs_lots(3), false).has_value());

	(void)legs.begin_replace(LEGS_ID);
	// The old leg's withdrawal, then the new leg's own fill counted from zero.
	(void)legs.on_report(LEGS_ID, 0, legs_lots(3), true);
	legs.resolve(LEGS_ID, replace_answer::replaced);
	const auto traded = legs.on_report(LEGS_ID, 1, legs_lots(2), false);

	ASSERT_TRUE(traded.has_value());
	EXPECT_EQ(*traded, legs_lots(5))
		<< "a leg's count restarts at zero; the order's must not";
}

TEST(SessionVenueLegs, ARepeatedOrStaleReportCannotMoveTheTotalBack) {
	venue_legs legs = legs_with_one_order();
	(void)legs.on_report(LEGS_ID, 0, legs_lots(4), false);

	const auto traded = legs.on_report(LEGS_ID, 0, legs_lots(1), false);
	ASSERT_TRUE(traded.has_value());
	EXPECT_EQ(*traded, legs_lots(4));
}

TEST(SessionVenueLegs, TheWorkingLegFinishingEndsTheChain) {
	venue_legs legs = legs_with_one_order();
	(void)legs.on_report(LEGS_ID, 0, legs_lots(1), true);
	EXPECT_EQ(legs.find(LEGS_ID), nullptr);
}

TEST(SessionVenueLegs, AFinishedLegWithAReplaceInFlightWaitsForTheAnswer) {
	venue_legs legs = legs_with_one_order();
	(void)legs.begin_replace(LEGS_ID);

	// Either the replace's own withdrawal of leg zero or a fill that will make
	// it fail; only the answer can say which.
	(void)legs.on_report(LEGS_ID, 0, legs_lots(0), true);
	ASSERT_NE(legs.find(LEGS_ID), nullptr);

	legs.resolve(LEGS_ID, replace_answer::replaced);
	ASSERT_NE(legs.find(LEGS_ID), nullptr);
	EXPECT_EQ(legs.working_leg(LEGS_ID), 1U);
	EXPECT_EQ(legs.find(LEGS_ID)->open_legs.size(), 0U)
		<< "the withdrawn leg is folded, not kept a slot per amendment";
}

TEST(SessionVenueLegs, AReplacementThatFilledBeforeItsAnswerEndsTheChain) {
	venue_legs legs = legs_with_one_order();
	(void)legs.begin_replace(LEGS_ID);
	(void)legs.on_report(LEGS_ID, 1, legs_lots(1), true);

	legs.resolve(LEGS_ID, replace_answer::replaced);
	EXPECT_EQ(legs.find(LEGS_ID), nullptr);
}

TEST(SessionVenueLegs, AnOrderAReplaceFoundGoneIsDropped) {
	for (const replace_answer answer :
		 {replace_answer::order_gone, replace_answer::withdrawn}) {
		venue_legs legs = legs_with_one_order();
		(void)legs.begin_replace(LEGS_ID);
		legs.resolve(LEGS_ID, answer);
		EXPECT_EQ(legs.find(LEGS_ID), nullptr);
	}
}

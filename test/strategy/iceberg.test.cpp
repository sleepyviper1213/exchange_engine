#include "strategy/iceberg.hpp"

#include "order_book/outcome.hpp"
#include "orders/types.hpp"
#include "strategy.fixture.hpp"
#include "strategy/command_batch.hpp"
#include "strategy/command_writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy;

namespace {

constexpr symbol_id_t ICEBERG_SYMBOL = 3;
constexpr order_id_t PARENT          = 500;
constexpr order_id_t CHILD_SEED      = 9000;
constexpr price_t ICEBERG_PRICE      = at_tick(100);

/// @brief An iceberg plus somewhere to write, since the two are always used
///        together and every test needs both.
struct Working {
	iceberg<4> ice{CHILD_SEED};
	command_batch<8> batch{ICEBERG_SYMBOL};

	command_writer &out() { return batch.writer(); }

	/// @brief Start a 500-lot parent showing 100 at a time.
	bool arm(quantity_t total = 500 * units::lot,
			 quantity_t peak  = 100 * units::lot) {
		return ice.arm(PARENT, side_t::bid, ICEBERG_PRICE, total, peak, out());
	}

	/// @brief The order carried by the @p i th command written so far.
	[[nodiscard]] const orders::order &placed(std::size_t i) const {
		return batch.view()[i].as_place();
	}
};

TEST(Iceberg, ArmingShowsTheFirstSliceImmediately) {
	Working w;
	
	ASSERT_TRUE(w.arm());

	ASSERT_EQ(w.batch.size(), 1U);
	EXPECT_EQ(w.batch.view()[0].type, event::command_type::PLACE);
	EXPECT_EQ(w.placed(0).id, CHILD_SEED);
	EXPECT_EQ(w.placed(0).qty, 100 * units::lot);
	EXPECT_EQ(w.placed(0).price, ICEBERG_PRICE);
	EXPECT_EQ(w.placed(0).side, side_t::bid);
	EXPECT_EQ(w.placed(0).symbol_id, ICEBERG_SYMBOL);
	EXPECT_EQ(w.ice.working(), 1U);
}

TEST(Iceberg, HidesEverythingBeyondThePeak) {
	Working w;
	ASSERT_TRUE(w.arm(500 * units::lot, 100 * units::lot));

	const auto parent = w.ice.parent(PARENT);
	ASSERT_TRUE(parent.has_value());
	EXPECT_EQ(parent->showing, 100 * units::lot);
	EXPECT_EQ(parent->reserve, 400 * units::lot);
	EXPECT_EQ(parent->peak, 100 * units::lot);
	EXPECT_EQ(parent->child, CHILD_SEED);
}

TEST(Iceberg, APeakAtOrAboveTheTotalIsJustAPlainOrder) {
	Working w;
	ASSERT_TRUE(w.arm(50 * units::lot, 100 * units::lot));

	EXPECT_EQ(w.placed(0).qty, 50 * units::lot);
	EXPECT_EQ(w.ice.parent(PARENT)->reserve, 0 * units::lot);
}

TEST(Iceberg, APartialFillLeavesTheSliceAloneAndShowsNothingMore) {
	Working w;
	ASSERT_TRUE(w.arm());

	w.ice.on_outcome(
		partially_filled(CHILD_SEED, 100 * units::lot, 30 * units::lot),
		w.out());

	EXPECT_EQ(w.batch.size(), 1U) << "a partial fill must not replenish";
	EXPECT_EQ(w.ice.parent(PARENT)->reserve, 400 * units::lot);
	EXPECT_EQ(w.ice.showing(PARENT), CHILD_SEED);
}

TEST(Iceberg, AFullyFilledSliceIsReplacedByTheNextOne) {
	Working w;
	ASSERT_TRUE(w.arm());

	w.ice.on_outcome(filled(CHILD_SEED, 100 * units::lot), w.out());

	ASSERT_EQ(w.batch.size(), 2U);
	EXPECT_EQ(w.placed(1).id, CHILD_SEED + 1) << "a fresh id per slice";
	EXPECT_EQ(w.placed(1).qty, 100 * units::lot);
	EXPECT_EQ(w.placed(1).price, ICEBERG_PRICE);
	EXPECT_EQ(w.ice.parent(PARENT)->reserve, 300 * units::lot);
	EXPECT_EQ(w.ice.showing(PARENT), CHILD_SEED + 1);
}

TEST(Iceberg, WorksTheWholeParentDownOneSliceAtATime) {
	Working w;
	ASSERT_TRUE(w.arm(500 * units::lot, 100 * units::lot));

	// Four more fills exhaust the reserve; the fifth ends the parent.
	for (int slice = 0; slice < 5; ++slice)
		w.ice.on_outcome(filled(CHILD_SEED + static_cast<order_id_t>(slice),
								100 * units::lot),
						 w.out());

	// Five slices placed in total, each of the peak size, and no sixth.
	ASSERT_EQ(w.batch.size(), 5U);
	for (std::size_t i = 0; i < 5; ++i)
		EXPECT_EQ(w.placed(i).qty, 100 * units::lot);
	EXPECT_EQ(w.ice.working(), 0U) << "the parent is done and its slot is free";
	EXPECT_FALSE(w.ice.parent(PARENT).has_value());
}

TEST(Iceberg, TheLastSliceIsTheRemainderNotAWholePeak) {
	Working w;
	ASSERT_TRUE(w.arm(250 * units::lot, 100 * units::lot));

	w.ice.on_outcome(filled(CHILD_SEED, 100 * units::lot), w.out());
	w.ice.on_outcome(filled(CHILD_SEED + 1, 100 * units::lot), w.out());

	ASSERT_EQ(w.batch.size(), 3U);
	EXPECT_EQ(w.placed(2).qty, 50 * units::lot) << "250 = 100 + 100 + 50";
	EXPECT_EQ(w.ice.parent(PARENT)->reserve, 0 * units::lot);
}

TEST(Iceberg, IgnoresOutcomesForOrdersItDoesNotOwn) {
	Working w;
	ASSERT_TRUE(w.arm());

	w.ice.on_outcome(filled(1, 100 * units::lot), w.out());
	w.ice.on_outcome(filled(CHILD_SEED + 99, 100 * units::lot), w.out());
	w.ice.on_outcome(
		order_outcome::cancel_rejected(7, reject_reason::UNKNOWN_ORDER),
		w.out());

	EXPECT_EQ(w.batch.size(), 1U);
	EXPECT_EQ(w.ice.parent(PARENT)->reserve, 400 * units::lot);
}

TEST(Iceberg, AnAcceptedSliceIsNotAnEventWorthActingOn) {
	Working w;
	ASSERT_TRUE(w.arm());

	w.ice.on_outcome(order_outcome::accepted(CHILD_SEED, 100 * units::lot),
					 w.out());

	EXPECT_EQ(w.batch.size(), 1U);
	EXPECT_EQ(w.ice.showing(PARENT), CHILD_SEED);
}

// Replenishing over a rejection would spin: whatever refused this slice refuses
// the next one too.
TEST(Iceberg, ARejectedSliceStopsTheParentRatherThanRetrying) {
	Working w;
	ASSERT_TRUE(w.arm());

	w.ice.on_outcome(order_outcome::rejected(CHILD_SEED,
											 reject_reason::BOOK_AT_CAPACITY,
											 100 * units::lot),
					 w.out());

	EXPECT_EQ(w.batch.size(), 1U);
	EXPECT_EQ(w.ice.working(), 0U);
}

TEST(Iceberg, ACancelledSliceEndsTheParent) {
	Working w;
	ASSERT_TRUE(w.arm());

	engine::order_state state{100 * units::lot};
	state.cancel();
	w.ice.on_outcome(order_outcome::cancelled(CHILD_SEED, state), w.out());

	EXPECT_EQ(w.batch.size(), 1U);
	EXPECT_EQ(w.ice.working(), 0U);
}

TEST(Iceberg, CancellingWithdrawsTheVisibleSliceAndForgetsTheReserve) {
	Working w;
	ASSERT_TRUE(w.arm());

	EXPECT_TRUE(w.ice.cancel(PARENT, w.out()));

	ASSERT_EQ(w.batch.size(), 2U);
	EXPECT_EQ(w.batch.view()[1].type, event::command_type::CANCEL);
	EXPECT_EQ(w.batch.view()[1].as_cancel(), CHILD_SEED);
	EXPECT_EQ(w.ice.working(), 0U);
}

TEST(Iceberg, CancellingSomethingItIsNotWorkingChangesNothing) {
	Working w;
	ASSERT_TRUE(w.arm());

	EXPECT_FALSE(w.ice.cancel(PARENT + 1, w.out()));
	EXPECT_EQ(w.batch.size(), 1U);
	EXPECT_EQ(w.ice.working(), 1U);
}

// After a cancel the slot is gone, so the fill that raced it must not
// replenish - the point of stopping is that nothing more is shown.
TEST(Iceberg, DoesNotReplenishAfterACancelEvenIfTheSliceThenFills) {
	Working w;
	ASSERT_TRUE(w.arm());
	ASSERT_TRUE(w.ice.cancel(PARENT, w.out()));

	w.ice.on_outcome(filled(CHILD_SEED, 100 * units::lot), w.out());

	EXPECT_EQ(w.batch.size(), 2U) << "the arm and the cancel, and nothing else";
}

TEST(Iceberg, RefusesMalformedParameters) {
	Working w;

	EXPECT_FALSE(w.ice.arm(0,
						   side_t::bid,
						   ICEBERG_PRICE,
						   500 * units::lot,
						   100 * units::lot,
						   w.out()))
		<< "id zero is the book's anonymous sentinel";
	EXPECT_FALSE(w.ice.arm(PARENT,
						   side_t::bid,
						   ICEBERG_PRICE,
						   0 * units::lot,
						   100 * units::lot,
						   w.out()));
	EXPECT_FALSE(w.ice.arm(PARENT,
						   side_t::bid,
						   ICEBERG_PRICE,
						   -5 * units::lot,
						   100 * units::lot,
						   w.out()));
	EXPECT_FALSE(w.ice.arm(PARENT,
						   side_t::bid,
						   ICEBERG_PRICE,
						   500 * units::lot,
						   0 * units::lot,
						   w.out()));
	EXPECT_EQ(w.batch.size(), 0U) << "a refusal emits nothing";
	EXPECT_EQ(w.ice.working(), 0U);
}

TEST(Iceberg, RefusesASecondParentUnderTheSameId) {
	Working w;
	ASSERT_TRUE(w.arm());

	EXPECT_FALSE(w.arm());
	EXPECT_EQ(w.batch.size(), 1U);
	EXPECT_EQ(w.ice.working(), 1U);
}

TEST(Iceberg, RefusesOnceEverySlotIsTaken) {
	iceberg<2> ice{CHILD_SEED};
	command_batch<8> batch{ICEBERG_SYMBOL};

	EXPECT_TRUE(ice.arm(1,
						side_t::bid,
						ICEBERG_PRICE,
						100 * units::lot,
						10 * units::lot,
						batch.writer()));
	EXPECT_TRUE(ice.arm(2,
						side_t::bid,
						ICEBERG_PRICE,
						100 * units::lot,
						10 * units::lot,
						batch.writer()));
	EXPECT_FALSE(ice.arm(3,
						 side_t::bid,
						 ICEBERG_PRICE,
						 100 * units::lot,
						 10 * units::lot,
						 batch.writer()));

	EXPECT_EQ(ice.working(), 2U);
	EXPECT_EQ(batch.size(), 2U);
}

// A retired slot has to be genuinely reusable, or a long session leaks capacity
// one finished parent at a time.
TEST(Iceberg, ReusesTheSlotOfAFinishedParent) {
	iceberg<1> ice{CHILD_SEED};
	command_batch<8> batch{ICEBERG_SYMBOL};

	ASSERT_TRUE(ice.arm(1,
						side_t::bid,
						ICEBERG_PRICE,
						10 * units::lot,
						10 * units::lot,
						batch.writer()));
	ice.on_outcome(filled(CHILD_SEED, 10 * units::lot), batch.writer());
	ASSERT_EQ(ice.working(), 0U);

	EXPECT_TRUE(ice.arm(2,
						side_t::ask,
						ICEBERG_PRICE,
						10 * units::lot,
						10 * units::lot,
						batch.writer()));
	EXPECT_EQ(ice.working(), 1U);
}

TEST(Iceberg, WorksSeveralParentsIndependently) {
	iceberg<4> ice{CHILD_SEED};
	command_batch<8> batch{ICEBERG_SYMBOL};

	ASSERT_TRUE(ice.arm(1,
						side_t::bid,
						at_tick(100),
						200 * units::lot,
						50 * units::lot,
						batch.writer()));
	ASSERT_TRUE(ice.arm(2,
						side_t::ask,
						at_tick(200),
						300 * units::lot,
						30 * units::lot,
						batch.writer()));

	// Fill the first parent's slice only.
	ice.on_outcome(filled(CHILD_SEED, 50 * units::lot), batch.writer());

	EXPECT_EQ(ice.parent(1)->reserve, 100 * units::lot);
	EXPECT_EQ(ice.parent(2)->reserve, 270 * units::lot)
		<< "untouched by another's fill";
	EXPECT_EQ(ice.parent(2)->child, CHILD_SEED + 1);
	EXPECT_EQ(ice.working(), 2U);
}

TEST(Iceberg, DeclaresABoundOfOneCommandPerOutcome) {
	static_assert(iceberg<4>::MAX_COMMANDS_PER_EVENT == 1);
	static_assert(iceberg<4>::MAX_WORKING == 4);
	SUCCEED();
}

} // namespace

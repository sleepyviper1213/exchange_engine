#include "strategy/stop.hpp"

#include "orders/order.hpp"
#include "orders/order_type.hpp"
#include "orders/types.hpp"
#include "strategy.fixture.hpp"
#include "strategy/command_writer.hpp"

#include <gtest/gtest.h>

#include <cstddef>


using namespace exchange;
using namespace exchange::engine;
using namespace exchange::strategy;

namespace {

constexpr symbol_id_t STOP_SYMBOL = 5;

/// @brief A buy stop: dormant until the market trades up to @p trigger, then a
///        limit at @p limit.
orders::order buy_stop(order_id_t id, price_t trigger, price_t limit,
					   quantity_t qty = 10) {
	return orders::order{.id         = id,
						 .side       = side_t::bid,
						 .type       = orders::order_type::STOP,
						 .price      = limit,
						 .stop_price = trigger,
						 .qty        = qty};
}

orders::order sell_stop(order_id_t id, price_t trigger, price_t limit,
						quantity_t qty = 10) {
	return orders::order{.id         = id,
						 .side       = side_t::ask,
						 .type       = orders::order_type::STOP,
						 .price      = limit,
						 .stop_price = trigger,
						 .qty        = qty};
}

struct Armed {
	stop<4> stops;
	command_batch<8> batch{STOP_SYMBOL};

	command_writer &out() { return batch.writer(); }

	[[nodiscard]] const orders::order &released(std::size_t i) const {
		return batch.view()[i].as_place();
	}
};

// ---------------------------------------------------------------------------
// The trigger rule, on its own
// ---------------------------------------------------------------------------

TEST(Stop, ABuyStopFiresAtOrAboveItsTrigger) {
	const orders::order o = buy_stop(1, 100, 105);

	EXPECT_FALSE(stop<4>::triggers(o, 99));
	EXPECT_TRUE(stop<4>::triggers(o, 100))
		<< "at the trigger, not just past it";
	EXPECT_TRUE(stop<4>::triggers(o, 101));
}

TEST(Stop, ASellStopFiresAtOrBelowItsTrigger) {
	const orders::order o = sell_stop(1, 100, 95);

	EXPECT_FALSE(stop<4>::triggers(o, 101));
	EXPECT_TRUE(stop<4>::triggers(o, 100));
	EXPECT_TRUE(stop<4>::triggers(o, 99));
}

// ---------------------------------------------------------------------------
// Arming
// ---------------------------------------------------------------------------

TEST(Stop, ArmingEmitsNothingBecauseThatIsWhatAStopIs) {
	Armed a;

	EXPECT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));

	EXPECT_EQ(a.batch.size(), 0U);
	EXPECT_EQ(a.stops.armed(), 1U);
	EXPECT_TRUE(a.stops.pending(1).has_value());
}

TEST(Stop, RefusesAnythingThatIsNotAWellFormedStop) {
	Armed a;

	orders::order not_a_stop = buy_stop(1, 100, 105);
	not_a_stop.type          = orders::order_type::LIMIT;
	EXPECT_FALSE(a.stops.arm(not_a_stop))
		<< "a LIMIT has no trigger to wait on";

	orders::order no_trigger = buy_stop(2, 0, 105);
	EXPECT_FALSE(a.stops.arm(no_trigger)) << "zero is the no-trigger sentinel";

	EXPECT_FALSE(a.stops.arm(buy_stop(0, 100, 105)))
		<< "id zero is the book's anonymous sentinel";
	EXPECT_FALSE(a.stops.arm(buy_stop(3, 100, 105, 0)));
	EXPECT_FALSE(a.stops.arm(buy_stop(4, 100, 105, -1)));

	EXPECT_EQ(a.stops.armed(), 0U);
}

TEST(Stop, RefusesASecondStopUnderTheSameId) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));

	EXPECT_FALSE(a.stops.arm(buy_stop(1, 200, 205)));
	EXPECT_EQ(a.stops.armed(), 1U);
	EXPECT_EQ(a.stops.pending(1)->stop_price, 100U) << "the first one survives";
}

TEST(Stop, RefusesOnceEverySlotIsTaken) {
	stop<2> stops;

	EXPECT_TRUE(stops.arm(buy_stop(1, 100, 105)));
	EXPECT_TRUE(stops.arm(buy_stop(2, 100, 105)));
	EXPECT_FALSE(stops.arm(buy_stop(3, 100, 105)));
	EXPECT_EQ(stops.armed(), 2U);
}

// ---------------------------------------------------------------------------
// Triggering
// ---------------------------------------------------------------------------

TEST(Stop, APrintShortOfTheTriggerReleasesNothing) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));

	a.stops.on_trade(strategy_print(99), a.out());

	EXPECT_EQ(a.batch.size(), 0U);
	EXPECT_EQ(a.stops.armed(), 1U);
}

TEST(Stop, ReleasesTheOrderAsALimitOnceTheTapeTradesThrough) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105, 25)));

	a.stops.on_trade(strategy_print(100), a.out());

	ASSERT_EQ(a.batch.size(), 1U);
	EXPECT_EQ(a.batch.view()[0].type, event::command_type::PLACE);
	EXPECT_EQ(a.released(0).id, 1U);
	EXPECT_EQ(a.released(0).qty, 25);
	EXPECT_EQ(a.released(0).price, 105U)
		<< "the limit it takes on, not the trigger";
	EXPECT_EQ(a.released(0).side, side_t::bid);
	EXPECT_EQ(a.released(0).symbol_id, STOP_SYMBOL);
}

// order_book refuses STOP outright, and validation refuses a non-stop that
// still carries a trigger. A release that kept either would be rejected.
TEST(Stop, TheReleasedOrderIsNoLongerAStop) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));

	a.stops.on_trade(strategy_print(100), a.out());

	ASSERT_EQ(a.batch.size(), 1U);
	EXPECT_EQ(a.released(0).type, orders::order_type::LIMIT);
	EXPECT_EQ(a.released(0).stop_price, 0U);
}

TEST(Stop, AReleasedStopIsGoneAndDoesNotFireTwice) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));

	a.stops.on_trade(strategy_print(100), a.out());
	a.stops.on_trade(strategy_print(150), a.out());
	a.stops.on_trade(strategy_print(200), a.out());

	EXPECT_EQ(a.batch.size(), 1U);
	EXPECT_EQ(a.stops.armed(), 0U);
	EXPECT_FALSE(a.stops.pending(1).has_value());
}

// One print can take out a whole cluster of stops at once - the cascade a stop
// run is made of, and the reason the per-event bound is the slot count.
TEST(Stop, OnePrintReleasesEveryStopItTriggers) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));
	ASSERT_TRUE(a.stops.arm(buy_stop(2, 110, 115)));
	ASSERT_TRUE(a.stops.arm(buy_stop(3, 120, 125)));
	ASSERT_TRUE(a.stops.arm(buy_stop(4, 130, 135)));

	a.stops.on_trade(strategy_print(125), a.out());

	EXPECT_EQ(a.batch.size(), 3U) << "triggers at 100, 110 and 120";
	EXPECT_EQ(a.stops.armed(), 1U);
	EXPECT_TRUE(a.stops.pending(4).has_value())
		<< "130 is still above the print";
}

TEST(Stop, BuyAndSellStopsAroundThePrintFireIndependently) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 110, 115))); // fires above 110
	ASSERT_TRUE(a.stops.arm(sell_stop(2, 90, 85)));  // fires below 90
	ASSERT_TRUE(a.stops.arm(buy_stop(3, 200, 205)));
	ASSERT_TRUE(a.stops.arm(sell_stop(4, 10, 5)));

	a.stops.on_trade(strategy_print(110), a.out());
	EXPECT_EQ(a.batch.size(), 1U);
	EXPECT_EQ(a.released(0).id, 1U);

	a.stops.on_trade(strategy_print(90), a.out());
	EXPECT_EQ(a.batch.size(), 2U);
	EXPECT_EQ(a.released(1).id, 2U);

	EXPECT_EQ(a.stops.armed(), 2U);
}

// ---------------------------------------------------------------------------
// Disarming
// ---------------------------------------------------------------------------

TEST(Stop, DisarmingWithdrawsAStopWithoutSendingAnything) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));

	EXPECT_TRUE(a.stops.disarm(1));

	EXPECT_EQ(a.batch.size(), 0U) << "nothing was ever placed to cancel";
	EXPECT_EQ(a.stops.armed(), 0U);

	a.stops.on_trade(strategy_print(200), a.out());
	EXPECT_EQ(a.batch.size(), 0U);
}

TEST(Stop, DisarmingSomethingUnknownChangesNothing) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));

	EXPECT_FALSE(a.stops.disarm(2));
	EXPECT_FALSE(a.stops.disarm(0));
	EXPECT_EQ(a.stops.armed(), 1U);
}

TEST(Stop, DisarmingAnAlreadyReleasedStopFails) {
	Armed a;
	ASSERT_TRUE(a.stops.arm(buy_stop(1, 100, 105)));
	a.stops.on_trade(strategy_print(100), a.out());

	EXPECT_FALSE(a.stops.disarm(1))
		<< "it is a resting order now; cancel it through the book";
}

TEST(Stop, ReusesTheSlotOfAReleasedStop) {
	stop<1> stops;
	command_batch<4> batch{STOP_SYMBOL};

	ASSERT_TRUE(stops.arm(buy_stop(1, 100, 105)));
	stops.on_trade(strategy_print(100), batch.writer());
	ASSERT_EQ(stops.armed(), 0U);

	EXPECT_TRUE(stops.arm(buy_stop(2, 200, 205)));
	EXPECT_EQ(stops.armed(), 1U);
}

TEST(Stop, DeclaresABoundOfOneCommandPerArmedSlot) {
	static_assert(stop<4>::MAX_COMMANDS_PER_EVENT == 4);
	static_assert(stop<16>::MAX_COMMANDS_PER_EVENT == 16);
	static_assert(stop<4>::MAX_ARMED == 4);
	SUCCEED();
}

} // namespace

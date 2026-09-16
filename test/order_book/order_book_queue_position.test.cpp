#include "matching_priority.fixture.hpp"
#include "order_book.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;
using namespace exchange;

// --------------------------------------------------------------------------
// Which orders have a queue position at all
// --------------------------------------------------------------------------

TEST(OrderBookQueuePosition, IsNulloptForAnythingNotResting) {
	order_book book;
	EXPECT_FALSE(book.queue_position_of(1).has_value()); // never placed

	priority_rest(book, 1, side_t::bid, 100, 10);
	book.add_order(side_t::bid, 100, 5); // anonymous: real depth, no id
	EXPECT_FALSE(book.queue_position_of(0).has_value());

	(void)book.place_order(
		{.id = 9, .side = side_t::ask, .price = 100, .qty = 10});
	EXPECT_FALSE(book.queue_position_of(1).has_value()) << "filled and gone";

	priority_rest(book, 2, side_t::bid, 100, 10);
	book.cancel_order(2);
	EXPECT_FALSE(book.queue_position_of(2).has_value()) << "cancelled and gone";
}

// --------------------------------------------------------------------------
// What the queue is measured against
// --------------------------------------------------------------------------

TEST(OrderBookQueuePosition, CountsOnlyTheOlderOrdersAtOurOwnPrice) {
	order_book book;
	const auto quotes =
		std::to_array<priority_quote>({{1, 10}, {2, 20}, {3, 30}});
	priority_rest_queue(book, side_t::bid, 100, quotes);
	priority_rest(book, 4, side_t::bid, 99, 40);  // worse price, not our queue
	priority_rest(book, 5, side_t::bid, 101, 50); // better price, ditto
	priority_rest(book, 6, side_t::ask, 105, 60); // other side entirely

	const std::optional<queue_position> queued = book.queue_position_of(2);
	ASSERT_TRUE(queued.has_value());
	EXPECT_EQ(queued->price, 100U);
	EXPECT_EQ(queued->side, side_t::bid);
	EXPECT_EQ(queued->remaining, 20);
	EXPECT_EQ(queued->lots_ahead, 10);
	EXPECT_EQ(queued->orders_ahead, 1U);
	EXPECT_EQ(queued->lots_behind, 30);
	EXPECT_EQ(queued->level_volume(), book.volume_at_price(100, side_t::bid));
	EXPECT_FALSE(queued->is_at_front());
}

TEST(OrderBookQueuePosition, TheOldestOrderAtAPriceIsAtTheFront) {
	order_book book;
	priority_rest_queue(book,
						side_t::bid,
						100,
						std::to_array<priority_quote>({{1, 10}, {2, 20}}));

	const std::optional<queue_position> first = book.queue_position_of(1);
	ASSERT_TRUE(first.has_value());
	EXPECT_TRUE(first->is_at_front());
	EXPECT_EQ(first->lots_ahead, 0);
	EXPECT_EQ(first->orders_ahead, 0U);
	EXPECT_EQ(first->lots_behind, 20);
}

TEST(OrderBookQueuePosition, AnonymousDepthIsQueueAheadLikeAnyOtherOrder) {
	order_book book;
	book.add_order(side_t::bid, 100, 50); // liquidity nobody can cancel
	priority_rest(book, 1, side_t::bid, 100, 10);

	const std::optional<queue_position> queued = book.queue_position_of(1);
	ASSERT_TRUE(queued.has_value());
	EXPECT_EQ(queued->lots_ahead, 50);
	EXPECT_EQ(queued->orders_ahead, 1U);
}

TEST(OrderBookQueuePosition, APartialFillCostsNoQueuePosition) {
	order_book book;
	priority_rest_queue(book,
						side_t::bid,
						100,
						std::to_array<priority_quote>({{1, 10}, {2, 10}}));

	// Takes 4 of the head order's 10 lots. It stays where it is - that is the
	// point - and the order behind it moves up by exactly what traded.
	(void)book.place_order(
		{.id = 9, .side = side_t::ask, .price = 100, .qty = 4});

	const std::optional<queue_position> head = book.queue_position_of(1);
	ASSERT_TRUE(head.has_value());
	EXPECT_TRUE(head->is_at_front());
	EXPECT_EQ(head->remaining, 6) << "a fill shrinks the order, not its place";

	const std::optional<queue_position> behind = book.queue_position_of(2);
	ASSERT_TRUE(behind.has_value());
	EXPECT_EQ(behind->lots_ahead, 6);
	EXPECT_EQ(behind->orders_ahead, 1U);
}

TEST(OrderBookQueuePosition, PolicyIsCarriedOnTheSnapshot) {
	order_book book{1U << 10, allocation_policy::PRO_RATA};
	priority_rest(book, 1, side_t::bid, 100, 10);

	const std::optional<queue_position> queued = book.queue_position_of(1);
	ASSERT_TRUE(queued.has_value());
	EXPECT_EQ(queued->policy, allocation_policy::PRO_RATA);
}

// --------------------------------------------------------------------------
// projected_fill - the question a passive quote actually has
// --------------------------------------------------------------------------

TEST(OrderBookQueuePosition, ProjectedFillIsZeroWithoutAnOrderOrASweep) {
	order_book book;
	priority_rest(book, 1, side_t::bid, 100, 10);

	EXPECT_EQ(book.projected_fill(7, 100), 0) << "no such order";
	EXPECT_EQ(book.projected_fill(1, 0), 0) << "nothing arriving";
	EXPECT_EQ(book.projected_fill(1, -5), 0) << "nor anything negative";
}

TEST(OrderBookQueuePosition, UnderPriceTimeOnlyWhatSurvivesTheQueueReachesUs) {
	order_book book;
	priority_rest_queue(book,
						side_t::bid,
						100,
						std::to_array<priority_quote>({{1, 10}, {2, 20}}));

	EXPECT_EQ(book.projected_fill(2, 5), 0) << "stops inside the order ahead";
	EXPECT_EQ(book.projected_fill(2, 10), 0) << "exactly clears it, no more";
	EXPECT_EQ(book.projected_fill(2, 15), 5);
	EXPECT_EQ(book.projected_fill(2, 30), 20);
	EXPECT_EQ(book.projected_fill(2, 500), 20) << "never more than we have";
}

TEST(OrderBookQueuePosition, UnderProRataTheBackOfTheQueueStillGetsAShare) {
	order_book book{1U << 10, allocation_policy::PRO_RATA};
	priority_rest_queue(book,
						side_t::bid,
						100,
						std::to_array<priority_quote>({{1, 10}, {2, 20}}));

	// A tenth of the level: 3.33 lots to the front order and 6.67 to ours,
	// floored to 3 and 6, with the residual lot going to the older order.
	EXPECT_EQ(book.projected_fill(1, 10), 4);
	EXPECT_EQ(book.projected_fill(2, 10), 6);
	EXPECT_EQ(book.projected_fill(1, 10) + book.projected_fill(2, 10), 10)
		<< "the shares are the whole sweep, not a fraction of it";

	// The same sweep under price-time would leave us nothing at all, which is
	// the entire difference the policy makes to a resting quote.
	order_book fifo;
	priority_rest_queue(fifo,
						side_t::bid,
						100,
						std::to_array<priority_quote>({{1, 10}, {2, 20}}));
	EXPECT_EQ(fifo.projected_fill(2, 10), 0);
}

TEST(OrderBookQueuePosition, BetterLevelsArePaidForBeforeTheSweepReachesUs) {
	order_book book;
	priority_rest(book, 1, side_t::bid, 101, 10); // the touch, ahead of us
	priority_rest(book, 2, side_t::bid, 100, 10);

	EXPECT_EQ(book.projected_fill(2, 10), 0) << "spent on the better level";
	EXPECT_EQ(book.projected_fill(2, 12), 2);
	EXPECT_EQ(book.projected_fill(2, 25), 10) << "capped at what we have";
}

TEST(OrderBookQueuePosition, TheProjectionIsWhatMatchingActuallyDoes) {
	// The claim the projection makes is that it runs the matcher's own
	// arithmetic, so the only test that can support it is one that asks and
	// then executes. Both policies, one book shape: a touch level that gets
	// swept whole and a second level that has to be divided.
	for (const allocation_policy policy :
		 {allocation_policy::PRICE_TIME, allocation_policy::PRO_RATA}) {
		order_book book{1U << 10, policy};
		priority_rest(book, 1, side_t::bid, 101, 10);
		priority_rest_queue(book,
							side_t::bid,
							100,
							std::to_array<priority_quote>({{2, 20}, {3, 30}}));

		constexpr volume_t SWEEP         = 45;
		const quantity_t projected_one   = book.projected_fill(1, SWEEP);
		const quantity_t projected_two   = book.projected_fill(2, SWEEP);
		const quantity_t projected_three = book.projected_fill(3, SWEEP);

		const std::vector<trade> trades = book.place_order(
			{.id = 9, .side = side_t::ask, .price = 100, .qty = SWEEP});

		EXPECT_EQ(priority_traded_for(trades, 1), projected_one)
			<< to_string(policy);
		EXPECT_EQ(priority_traded_for(trades, 2), projected_two)
			<< to_string(policy);
		EXPECT_EQ(priority_traded_for(trades, 3), projected_three)
			<< to_string(policy);
		EXPECT_EQ(projected_one + projected_two + projected_three, SWEEP)
			<< to_string(policy) << ": the sweep is fully allocated";
	}
}

TEST(OrderBookQueuePosition, TheProjectionMovesAsTheQueueInFrontIsWorkedOff) {
	// What a resting quote watches: the same sweep that reaches nothing today
	// fills us once the orders in front have traded away.
	order_book book;
	priority_rest_queue(book,
						side_t::bid,
						100,
						std::to_array<priority_quote>({{1, 10}, {2, 10}}));
	ASSERT_EQ(book.projected_fill(2, 8), 0);

	(void)book.place_order(
		{.id = 9, .side = side_t::ask, .price = 100, .qty = 6});
	EXPECT_EQ(book.projected_fill(2, 8), 4) << "4 lots of the queue ahead left";

	(void)book.place_order(
		{.id = 8, .side = side_t::ask, .price = 100, .qty = 4});
	const std::optional<queue_position> queued = book.queue_position_of(2);
	ASSERT_TRUE(queued.has_value());
	EXPECT_TRUE(queued->is_at_front());
	EXPECT_EQ(book.projected_fill(2, 8), 8) << "front of the queue now";
}

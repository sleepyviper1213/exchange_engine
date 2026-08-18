#include "trading-engine/event/engine_event.hpp"
#include "trading-engine/event/event_channel.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

// The wire itself: staging a drained batch into symbol-stamped events, and
// getting all of them across a ring that is allowed to be too small. Two
// properties carry the whole component - nothing is dropped, and nothing is
// reordered - and both are only interesting when the ring fills, so most of this
// file works with a ring of four.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;

namespace {

trade print(order_id_t aggressor, quantity_t volume) {
	return {.aggressor = aggressor, .resting = 99, .price = 100, .volume = volume};
}

// Everything the host side can see, in the order it arrived.
std::vector<engine_event> receive_all(event_channel<4> &channel) {
	std::vector<engine_event> seen;
	std::array<engine_event, 8> buffer{};
	for (std::size_t n = channel.receive(std::span(buffer)); n != 0;
		 n             = channel.receive(std::span(buffer)))
		seen.insert(seen.end(), buffer.begin(), buffer.begin() + n);
	return seen;
}

TEST(EventChannel, AnEmptyChannelYieldsNothing) {
	event_channel<4> channel;
	std::array<engine_event, 4> buffer{};
	EXPECT_EQ(channel.receive(std::span(buffer)), 0U);
	EXPECT_FALSE(channel.has_pending());
	EXPECT_EQ(channel.published(), 0U);
}

// The cut list is what turns two anonymous buffers into per-listing streams.
TEST(EventChannel, PublishStampsEachSliceWithItsListing) {
	event_channel<4> channel;
	const std::array trades{print(1, 4), print(2, 6)};
	const std::array outcomes{order_outcome::accepted(1, 4)};
	// Listing 7 produced the first trade; listing 8 the second trade and the
	// outcome.
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 1, .outcome_end = 0},
		symbol_run{.symbol = 8, .trade_end = 2, .outcome_end = 1}};

	ASSERT_TRUE(channel.publish(runs, trades, outcomes));
	EXPECT_EQ(channel.published(), 3U);

	const std::vector<engine_event> seen = receive_all(channel);
	ASSERT_EQ(seen.size(), 3U);
	EXPECT_EQ(seen[0], engine_event::of(7, trades[0]));
	EXPECT_EQ(seen[1], engine_event::of(8, trades[1]));
	EXPECT_EQ(seen[2], engine_event::of(8, outcomes[0]));
}

// A listing's trades precede its outcomes, so a reader can apply the order state
// to the execution that caused it rather than the other way round. This is the
// property two separate rings could not offer, and the reason there is one.
TEST(EventChannel, ATradePrecedesTheOutcomeThatExplainsIt) {
	event_channel<4> channel;
	const std::array trades{print(1, 4)};
	const std::array outcomes{order_outcome::accepted(1, 4)};
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 1, .outcome_end = 1}};

	ASSERT_TRUE(channel.publish(runs, trades, outcomes));

	const std::vector<engine_event> seen = receive_all(channel);
	ASSERT_EQ(seen.size(), 2U);
	EXPECT_EQ(seen[0].kind, EventKind::TRADE);
	EXPECT_EQ(seen[1].kind, EventKind::OUTCOME);
}

// A command that published nothing gets no slice, so an idle drain costs the
// channel nothing at all.
TEST(EventChannel, ABatchWithNoRunsPublishesNothing) {
	event_channel<4> channel;
	ASSERT_TRUE(channel.publish({}, {}, {}));
	EXPECT_EQ(channel.published(), 0U);
	EXPECT_FALSE(channel.has_pending());
}

// The one behaviour the design turns on. Six events into a ring of four: the
// first four go, the remainder is held, and retry finishes the job once the host
// has made room. Nothing is dropped and nothing is reordered - a full ring is
// back-pressure, never eviction, because a lost print would make the tape a
// function of scheduling.
TEST(EventChannel, AFullRingHoldsTheRemainderInsteadOfDroppingIt) {
	event_channel<4> channel;
	std::array<trade, 6> trades{};
	for (std::size_t i = 0; i < trades.size(); ++i)
		trades[i] = print(static_cast<order_id_t>(i + 1), 1);
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 6, .outcome_end = 0}};

	EXPECT_FALSE(channel.publish(runs, trades, {}));
	EXPECT_TRUE(channel.has_pending());
	EXPECT_EQ(channel.backlog(), 2U);
	EXPECT_EQ(channel.published(), 4U);
	EXPECT_EQ(channel.stalls(), 1U);

	// Still stuck while the host has taken nothing.
	EXPECT_FALSE(channel.retry());
	EXPECT_EQ(channel.backlog(), 2U);

	std::array<engine_event, 4> buffer{};
	ASSERT_EQ(channel.receive(std::span(buffer)), 4U);
	EXPECT_TRUE(channel.retry());
	EXPECT_FALSE(channel.has_pending());
	EXPECT_EQ(channel.published(), 6U);

	// And the two that waited are the two that were last, in order.
	ASSERT_EQ(channel.receive(std::span(buffer)), 2U);
	EXPECT_EQ(buffer[0], engine_event::of(7, trades[4]));
	EXPECT_EQ(buffer[1], engine_event::of(7, trades[5]));
}

// retry() with no backlog is vacuously done, so a caller can put it at the top of
// its loop without asking whether it is needed.
TEST(EventChannel, RetryWithNoBacklogSucceeds) {
	event_channel<4> channel;
	EXPECT_TRUE(channel.retry());
	EXPECT_EQ(channel.published(), 0U);
	EXPECT_EQ(channel.stalls(), 0U);
}

// A batch far larger than the ring still arrives whole, in order, one ring-full
// at a time. This is the case that would have to drop events if publish were
// best-effort.
TEST(EventChannel, ABatchSeveralTimesTheRingArrivesWholeAndInOrder) {
	event_channel<4> channel;
	std::array<trade, 19> trades{};
	for (std::size_t i = 0; i < trades.size(); ++i)
		trades[i] = print(static_cast<order_id_t>(i + 1), 1);
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 19, .outcome_end = 0}};

	std::vector<engine_event> seen;
	std::array<engine_event, 4> buffer{};
	bool done = channel.publish(runs, trades, {});
	while (!done) {
		const std::size_t n = channel.receive(std::span(buffer));
		ASSERT_GT(n, 0U) << "the ring must hold something for retry to free";
		seen.insert(seen.end(), buffer.begin(), buffer.begin() + n);
		done = channel.retry();
	}
	for (std::size_t n = channel.receive(std::span(buffer)); n != 0;
		 n             = channel.receive(std::span(buffer)))
		seen.insert(seen.end(), buffer.begin(), buffer.begin() + n);

	ASSERT_EQ(seen.size(), trades.size());
	for (std::size_t i = 0; i < trades.size(); ++i)
		EXPECT_EQ(seen[i], engine_event::of(7, trades[i])) << "at " << i;
	EXPECT_EQ(channel.published(), trades.size());
}

// receive takes at most what the caller offered room for, and leaves the rest
// queued rather than failing.
TEST(EventChannel, ReceiveTakesAtMostTheBufferItWasGiven) {
	event_channel<4> channel;
	const std::array trades{print(1, 1), print(2, 1), print(3, 1)};
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 3, .outcome_end = 0}};
	ASSERT_TRUE(channel.publish(runs, trades, {}));

	std::array<engine_event, 2> small{};
	EXPECT_EQ(channel.receive(std::span(small)), 2U);
	EXPECT_EQ(channel.queued(), 1U);
	EXPECT_EQ(channel.receive(std::span(small)), 1U);
	EXPECT_EQ(channel.queued(), 0U);
}

} // namespace

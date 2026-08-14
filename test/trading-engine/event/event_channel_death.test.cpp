#include "trading-engine/event/engine_event.hpp"
#include "trading-engine/event/event_channel.hpp"
#include "trading-engine/order_book/trade.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

// The channel's contracts that only fire in a build with assertions live. Kept in
// their own file because a suite that forks should not share a binary state with
// one that does not. @see testing.md

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;

namespace {

std::array<trade, 6> six_prints() {
	std::array<trade, 6> trades{};
	for (std::size_t i = 0; i < trades.size(); ++i)
		trades[i] = {.aggressor = static_cast<order_id_t>(i + 1),
					 .resting   = 99,
					 .price     = 100,
					 .volume    = 1};
	return trades;
}

// Publishing over an unfinished batch would put the newer events in front of the
// older ones — the one thing the whole component exists to prevent — so it is a
// contract violation and not a case to handle. The caller's obligation is to
// retry() until the backlog clears.
TEST(EventChannelDeath, PublishingOverABacklogIsAContractViolation) {
	event_channel<4> channel;
	const std::array<trade, 6> trades = six_prints();
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 6, .outcome_end = 0}};

	ASSERT_FALSE(channel.publish(runs, trades, {}));
	ASSERT_TRUE(channel.has_pending());

	EXPECT_DEATH(static_cast<void>(channel.publish(runs, trades, {})),
				 "clear the backlog");
}

// A cut list whose offsets run past the buffers it cuts would read out of bounds.
// The offsets come from the partition that built both, so disagreement is a bug in
// the caller and worth failing loudly for rather than clamping.
TEST(EventChannelDeath, ARunReachingPastTheBatchIsAContractViolation) {
	event_channel<4> channel;
	const std::array trades{trade{
		.aggressor = 1, .resting = 99, .price = 100, .volume = 1}};
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 2, .outcome_end = 0}};

	EXPECT_DEATH(static_cast<void>(channel.publish(runs, trades, {})),
				 "must lie inside the batch");
}

} // namespace

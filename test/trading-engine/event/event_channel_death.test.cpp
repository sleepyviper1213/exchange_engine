#include "event/engine_event.hpp"
#include "event/event_channel.hpp"
#include "order_book/trade.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>

// The channel's contracts that only fire while assertions are live. Kept in
// their own file because a suite that forks should not share a binary with one
// that does not. @see testing.md
//
// Both are preconditions in the CLAUDE.md sense - an invariant the *caller*
// guarantees, not input validation - because both offsets and the publish order
// come from engine_partition, which builds them itself. Neither is reachable
// from anything a client sends.

using namespace exchange;
using namespace exchange::engine;
using namespace exchange::engine::event;
using testing::HasSubstr;

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

// Publishing over an unfinished batch would put the newer events in front of
// the older ones - the one thing the whole component exists to prevent - so it
// is a contract violation and not a case to handle. The caller's obligation is
// to retry() until the backlog clears.
TEST(EventChannelDeath, PublishingOverABacklogIsAContractViolation) {
	event_channel<4> channel;
	const std::array<trade, 6> trades = six_prints();
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 6, .outcome_end = 0}};

	ASSERT_FALSE(channel.publish(runs, trades, {}));
	ASSERT_TRUE(channel.has_pending());

	// DEBUG_DEATH rather than DEATH, this reorders a stream nobody is reading
	// - so running it in-process is safe.
	EXPECT_DEBUG_DEATH((void)channel.publish(runs, trades, {}),
					   HasSubstr("clear the backlog"));
}

// A cut list whose offsets run past the buffers it cuts would read out of
// bounds. The offsets come from the partition that built both, so disagreement
// is a bug in the caller and worth failing loudly for rather than clamping.
TEST(EventChannelDeath, ARunReachingPastTheBatchIsAContractViolation) {
	event_channel<4> channel;
	const std::array trades{
		trade{.aggressor = 1, .resting = 99, .price = 100, .volume = 1}};
	const std::array runs{
		symbol_run{.symbol = 7, .trade_end = 2, .outcome_end = 0}};

#ifdef NDEBUG
	GTEST_SKIP()
		<< "assertions are compiled out (ORDER_BOOK_ENABLE_HARDENING is "
		   "OFF and this configuration defines NDEBUG); executing the "
		   "statement would be an out-of-bounds read, not a wrong answer";
#else
	EXPECT_DEATH((void)channel.publish(runs, trades, {}),
				 HasSubstr("must lie inside the batch"));
#endif
}

} // namespace

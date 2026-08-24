#include "live_session.fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>

// Passive execution on the live path.
//
// `AppLiveSession.APassiveQuoteNeverTrades` is the other half of this file and
// should be read with it: it pins that a resting quote fills against nothing
// when the depth mirrored into the book is rested with `add_order`, which does
// not match. That is a fact about how a venue's depth is seeded, not a finding
// about the strategy, and it is what `simulate_fills` exists to answer.

using namespace exchange;
using namespace exchange::session;

namespace {

/// @brief Ticks the market drops in one frame - far enough that the quoter
///        cannot follow it.
constexpr std::int64_t LIVE_GAP_TICKS = 10;

/**
 * @brief Move the market down through the resting quote in a single frame.
 *
 * @par Why one big move and not a gentle walk
 * Worth stating, because the obvious market does not work and the reason is a
 * real property of the strategy rather than of the model. The reference quoter
 * requotes off *every* frame, so a touch stepping down one tick at a time keeps
 * the venue's ask a fixed two ticks above our bid forever: our order is never
 * stale, and an order that is never stale is never traded through. A resting
 * order fills when the market moves further than the quote can follow.
 *
 * @par Why the old touch has to be withdrawn in the same frame
 * A diff *updates* levels rather than replacing a book, so publishing 90/94
 * without zeroing 100/104 leaves the replica holding a bid of 100 and an ask of
 * 94 - crossed. @c l2_book counts that as crossed, the reconstructor tears a
 * crossed replica down (@c reconstructor_options::resync_on_cross), and a
 * session with no live replica neither quotes nor infers. The test would then
 * pass or fail for a reason that has nothing to do with fills.
 */
void gap_down_through_the_quote(live_desk &desk,
								market_data::sequence_t at = 2) {
	const std::int64_t bid = LIVE_TOUCH_BID - LIVE_GAP_TICKS;
	const std::int64_t ask = LIVE_TOUCH_ASK - LIVE_GAP_TICKS;
	desk.frame(diff(at,
					0,
					{level(LIVE_TOUCH_BID, 0), level(bid, 5)},
					{level(LIVE_TOUCH_ASK, 0), level(ask, 5)}));
}

/// @brief A session that infers passive fills, with everything else default.
[[nodiscard]] live_session_options simulating() {
	live_session_options options;
	options.simulate_fills = true;
	return options;
}

} // namespace

// --- the default, which must not have moved -------------------------------

TEST(AppLiveSessionFills, TheDefaultInfersNothing) {
	live_desk desk;
	ASSERT_TRUE(desk.seed_touch());
	gap_down_through_the_quote(desk);

	EXPECT_EQ(desk.report().injected_aggressors, 0U);
	EXPECT_EQ(desk.report().injected_lots, 0);
	EXPECT_EQ(desk.report().queue_absorbed_lots, 0);
	EXPECT_EQ(desk.fills(), 0U)
		<< "`serve` without the flag is the production chain and nothing "
		   "simulated - the same run it was before the model was wired in";
}

// --- the counter that tells a quiet market from a dead pipeline ------------

TEST(AppLiveSessionFills, TheFrameCountSeparatesLookedAndFoundNothingFromNever) {
	// A market that never crosses our quote: the touch holds still, so there is
	// nothing to trade through and every *outcome* counter reads zero - exactly
	// what a pipeline that had stopped inferring would also report.
	live_desk quiet{simulating()};
	ASSERT_TRUE(quiet.seed_touch());
	for (market_data::sequence_t at = 2; at <= 5; ++at)
		quiet.move_touch(at, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);

	ASSERT_EQ(quiet.report().injected_aggressors, 0U) << "nothing crossed us";
	EXPECT_GT(quiet.report().inferred_frames, 0U)
		<< "but the model looked on every frame, which is the only thing that "
		   "distinguishes this run from one where it had stopped running";

	live_desk off;
	ASSERT_TRUE(off.seed_touch());
	off.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);
	EXPECT_EQ(off.report().inferred_frames, 0U)
		<< "and with the flag clear it never looks, so the two cases really do "
		   "print different reports";
}

TEST(AppLiveSessionFills, ADeadReplicaIsNotInferredAgainst) {
	live_desk desk{simulating()};
	ASSERT_TRUE(desk.seed_touch());
	desk.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);
	const std::uint64_t before = desk.report().inferred_frames;
	ASSERT_GT(before, 0U);

	ASSERT_EQ(desk.move_touch(9, LIVE_TOUCH_BID, LIVE_TOUCH_ASK),
			  market_data::sequence_action::gap);

	EXPECT_EQ(desk.report().inferred_frames, before)
		<< "a replica out of sequence is not evidence about the venue, and the "
		   "depth it seeded has already been withdrawn from the book - so the "
		   "frame must not count as one the model was able to judge";
}

// --- what the flag buys ----------------------------------------------------

TEST(AppLiveSessionFills, ASimulatedFillLetsAPassiveQuoteTrade) {
	live_desk desk{simulating()};
	ASSERT_TRUE(desk.seed_touch());
	ASSERT_EQ(desk.session().quoter().quotes(), 2U)
		<< "two sides resting inside the touch, and neither of them a take";
	ASSERT_EQ(desk.session().quoter().takes(), 0U);

	gap_down_through_the_quote(desk);

	EXPECT_GT(desk.report().injected_aggressors, 0U)
		<< "the venue offered below our resting bid, so somebody was willing "
		   "to sell lower than we were willing to buy";
	EXPECT_GT(desk.fills(), 0U)
		<< "counted at the far end of the whole loop by the post-trade monitor, "
		   "so this is a fill the engine really matched rather than a number "
		   "the model reported about itself";
	EXPECT_GT(desk.net(), 0) << "and we are long, because it was the bid";
	EXPECT_EQ(desk.session().quoter().takes(), 0U)
		<< "and we never crossed a spread to get it - the whole point is that "
		   "this fill is passive";
}

TEST(AppLiveSessionFills, AnInjectedAggressorIsNeverScreenedByTheGate) {
	live_desk desk{simulating()};
	ASSERT_TRUE(desk.seed_touch());
	gap_down_through_the_quote(desk);
	ASSERT_GT(desk.report().injected_aggressors, 0U);

	EXPECT_EQ(desk.session().gate().refused(), 0U)
		<< "nothing was refused, so nothing injected was even offered - the "
		   "venue's own flow has no business being screened against our limits";
	EXPECT_LE(desk.session().gate().working_orders(), 2U)
		<< "and the gate's ledger holds only the quoter's own sides. An "
		   "injection reaching it would be counted as our exposure and would "
		   "ratchet the position limit closed over a run";
}

TEST(AppLiveSessionFills, TheAggressorReachesTheStaleQuoteBeforeTheRequote) {
	live_desk desk{simulating()};
	ASSERT_TRUE(desk.seed_touch());
	const auto stale_bid = desk.book().best_bid();
	ASSERT_TRUE(stale_bid.has_value());
	ASSERT_EQ(*stale_bid, LIVE_TOUCH_BID + 1);

	gap_down_through_the_quote(desk);

	// The frame's queue order is depth, then the aggressor, then the quoter's
	// cancel and replace. If it were the other way round the aggressor would
	// arrive at a price we had already left and match nothing, which is exactly
	// what happens if `inject` is moved after `quote`.
	EXPECT_GT(desk.fills(), 0U);
	EXPECT_LT(desk.book().best_bid(), *stale_bid)
		<< "and by the end of the frame the quote has followed the market down, "
		   "so the fill can only have happened against the old one";
}

// --- queue position --------------------------------------------------------

TEST(AppLiveSessionFills, RestingQuotesAreMeasuredAgainstTheVenuesQueue) {
	live_desk desk{simulating()};
	ASSERT_TRUE(desk.seed_touch());

	// Not after the snapshot: `inject` runs before `quote`, so on the frame that
	// places our first quotes there is nothing yet to measure. The frame after
	// is the first one that can see them.
	desk.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);

	EXPECT_GT(desk.session().fills().queue().tracked(), 0U)
		<< "both quotes are behind the venue's touch, so both are measured";
}

TEST(AppLiveSessionFills, TheQueueEstimateIsAbandonedOnAGap) {
	live_desk desk{simulating()};
	ASSERT_TRUE(desk.seed_touch());
	desk.move_touch(2, LIVE_TOUCH_BID, LIVE_TOUCH_ASK);
	ASSERT_GT(desk.session().fills().queue().tracked(), 0U);

	// Sequence 9 against a replica expecting 3: a gap, which tears the replica
	// down and withdraws the depth it had seeded.
	ASSERT_EQ(desk.move_touch(9, LIVE_TOUCH_BID, LIVE_TOUCH_ASK),
			  market_data::sequence_action::gap);

	EXPECT_EQ(desk.session().fills().queue().tracked(), 0U)
		<< "every estimate was measured against a replica that no longer "
		   "exists; re-measuring is the conservative choice as well as the "
		   "simple one";
}

TEST(AppLiveSessionFills, AQuoteInsideTheTouchHasNothingQueuedAheadOfIt) {
	live_desk desk{simulating()};
	ASSERT_TRUE(desk.seed_touch());
	gap_down_through_the_quote(desk);
	ASSERT_GT(desk.fills(), 0U);

	EXPECT_EQ(desk.report().queue_absorbed_lots, 0)
		<< "the reference quoter improves on the touch, so it rests at a price "
		   "the venue publishes nothing at - there is no venue queue in front "
		   "of it to pay down. Which is worth pinning rather than assuming: it "
		   "means `--front-of-queue` changes nothing for *this* strategy, and a "
		   "run that wants the queue model to bite has to quote *at* the touch";
}

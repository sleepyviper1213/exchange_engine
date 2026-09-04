#include "live_session.fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// Execution through the real two-thread pipeline, at speed.
//
// Every other live suite drives the producer and the consumer from one thread
// in sequence. That is the right shape for saying *which frame produced which
// fill*, and it is why those suites exist - but it means none of them ever runs
// the hand-off it is testing. Here a real consumer thread is spawned and the
// producer feeds frames as fast as it can, so the queue fills, the channel
// backs up, and both sides take their retry paths.
//
// --- what this can and cannot claim ---------------------------------------
//
// Nothing here asserts a count, because under a real interleaving there is no
// right count: how many frames the consumer has applied when the producer
// writes the next one is a scheduling question. So every assertion below is an
// invariant that has to hold for *every* interleaving, and the stress loop
// exists to visit enough of them to make a violated one likely rather than
// possible.
//
// What it would look like if it failed: an outcome lost on the return path
// shows up as the gate's ledger growing with the frame count, because entries
// retire on outcomes and nothing else; a command applied twice shows up as a
// position larger than every lot the model ever offered us; a deadlock shows up
// as the suite hanging rather than as a failed expectation, which is why the
// shutdown handshake below is written the way it is and commented.
//
// What it would *not* catch, checked rather than assumed: a command left on the
// ring. The consumer comfortably keeps up with a producer that runs a bridge, a
// gate and a fill model per frame, so the ring is empty at the end whether or
// not the consumer's final drain runs at all - deleting that loop leaves every
// assertion here green. The depth check is kept as a quiescence sanity check
// and is deliberately not described as more than that.
//
// **This is a stress test, not a proof.** It has not been run under
// ThreadSanitizer, because no TSan preset is offered on Windows and this is
// where it was written. A TSan run on macOS or Linux is still owed.

using namespace exchange;
using namespace exchange::session;
using exchange::market_data::book_level;

namespace {

/// @brief The two touches the market alternates between.
///
/// Far enough apart that the quoter cannot follow in one frame, which is what
/// makes a resting quote stale and therefore fillable. Alternating rather than
/// walking one way so both sides fill over a run: dropping to the low touch
/// trades through our bid, coming back up trades through our ask.
constexpr std::int64_t REALTIME_LOW_BID = 90;
constexpr std::int64_t REALTIME_LOW_ASK = 94;
constexpr std::int64_t REALTIME_LOTS    = 5;

/// @brief Frames per run. Enough that the SPSC queue and the event channel both
///        reach their retry paths on a normal machine, and short enough that
///        the suite stays a few milliseconds.
constexpr int REALTIME_FRAMES = 400;

/// @brief Independent runs. A single run visits one interleaving; a concurrency
///        test that runs once is a test of one schedule.
constexpr int REALTIME_RUNS = 8;

/// @brief One diff moving the touch to @p bid / @p ask and withdrawing the
///        other pair, so the replica is never crossed. @see the fills suite for
///        why a crossed replica silently disables the whole path.
[[nodiscard]] market_data::depth_event touch_frame(market_data::sequence_t at,
												   bool low) {
	const std::int64_t bid      = low ? REALTIME_LOW_BID : LIVE_TOUCH_BID;
	const std::int64_t ask      = low ? REALTIME_LOW_ASK : LIVE_TOUCH_ASK;
	const std::int64_t gone_bid = low ? LIVE_TOUCH_BID : REALTIME_LOW_BID;
	const std::int64_t gone_ask = low ? LIVE_TOUCH_ASK : REALTIME_LOW_ASK;
	const auto bids             = std::to_array<book_level>(
		{level(gone_bid, 0), level(bid, REALTIME_LOTS)});
	const auto asks = std::to_array<book_level>(
		{level(gone_ask, 0), level(ask, REALTIME_LOTS)});
	return diff(at, 0, bids, asks);
}

/// @brief What one run of the real pipeline produced.
struct realtime_outcome {
	std::uint64_t fills          = 0;
	std::uint64_t injected       = 0;
	volume_t injected_lots       = 0;
	std::uint64_t quotes         = 0;
	std::uint64_t takes          = 0;
	volume_t net                 = 0;
	std::uint64_t engine_events  = 0;
	std::size_t left_queued      = 0;
	std::uint32_t working_orders = 0;
	int frames                   = 0;
};

/**
 * @brief Run the live pipeline over @p frames with a real consumer thread.
 *
 * @par The shutdown handshake, which is the only delicate part
 * `drain_and_publish` blocks on a full event channel until the producer reads
 * it, so a producer that joined the consumer while the channel was full would
 * deadlock: the consumer cannot finish publishing and the producer is not
 * pumping. Hence two flags rather than one. `stop` tells the consumer to finish
 * its backlog; `done` tells the producer the backlog is finished; and the
 * producer keeps pumping until it sees `done` and only then joins.
 *
 * @par Memory ordering
 * Both flags are release/acquire, which is exactly what a handshake needs and
 * no more: the store has to be visible to the other thread's load, and neither
 * flag guards data. The commands and events themselves are ordered by the SPSC
 * queue and the event channel, which do their own synchronisation and are
 * tested for it in their own suites. Sequential consistency here would buy a
 * fence on the store and change nothing that is observable.
 */
[[nodiscard]] realtime_outcome run_realtime(int frames,
											live_session_options options) {
	const engine::symbol_spec spec = unit_listing();
	manual_clock clock;
	test_live_session run{spec, options, clock};

	std::atomic<bool> stop{false};
	std::atomic<bool> done{false};

	std::thread consumer([&run, &stop, &done] {
		while (!stop.load(std::memory_order_acquire))
			if (run.drain_and_publish() == 0) std::this_thread::yield();
		// Whatever the producer wrote after the flag was read still has to be
		// applied, or the "nothing was lost" assertion below would be measuring
		// an early exit rather than a drain.
		while (run.drain_and_publish() > 0) {}
		done.store(true, std::memory_order_release);
	});

	run.on_snapshot(seed(
		1,
		std::to_array<book_level>({level(LIVE_TOUCH_BID, REALTIME_LOTS)}),
		std::to_array<book_level>({level(LIVE_TOUCH_ASK, REALTIME_LOTS)})));

	for (int frame = 0; frame < frames; ++frame) {
		const auto at = static_cast<market_data::sequence_t>(frame + 2);
		run.on_event(touch_frame(at, frame % 2 == 0));
	}

	stop.store(true, std::memory_order_release);
	while (!done.load(std::memory_order_acquire)) run.pump();
	consumer.join();
	run.pump_all();

	return realtime_outcome{
		.fills         = run.monitor().fills().total_executions(),
		.injected      = run.report().injected_aggressors,
		.injected_lots = run.report().injected_lots,
		.quotes        = run.quoter().quotes(),
		.takes         = run.quoter().takes(),
		.net           = run.positions().net_lots(spec.id()),
		.engine_events = run.report().engine_events,
		// From this thread, which the threading contract reserves for the
		// consumer - legal only because the consumer has been joined above and
		// this is now the single thread in the process. It cannot block: the
		// channel was emptied by `pump_all` on the line before.
		.left_queued    = run.drain_and_publish(),
		.working_orders = run.gate().working_orders(),
		.frames         = frames,
	};
}

[[nodiscard]] live_session_options realtime_simulating() {
	live_session_options options;
	options.simulate_fills = true;
	return options;
}

} // namespace

TEST(AppLiveSessionRealtime,
	 APassiveQuoteFillsThroughTheRealTwoThreadPipeline) {
	const realtime_outcome result =
		run_realtime(REALTIME_FRAMES, realtime_simulating());

	EXPECT_GT(result.quotes, 0U) << "the quoter ran at all";
	EXPECT_GT(result.injected, 0U)
		<< "the market moved through resting quotes, so the model inferred "
		   "trades against them";
	EXPECT_GT(result.fills, 0U)
		<< "and the matching engine - on its own thread, draining a queue the "
		   "producer was still writing to - really matched them";
	EXPECT_EQ(result.takes, 0U) << "none of it was bought by crossing a spread";
}

TEST(AppLiveSessionRealtime, TheReturnPathRetiresEveryOrderItAccepts) {
	const realtime_outcome result =
		run_realtime(REALTIME_FRAMES, realtime_simulating());

	ASSERT_GT(result.quotes, 0U);
	EXPECT_GT(result.engine_events, 0U) << "the return path carried something";

	// The falsifiable one, and the reason it is phrased against the *ledger*
	// rather than against a queue depth. The gate inserts on accept and retires
	// on a terminal outcome, so an outcome that never arrives leaves an entry
	// behind for ever. Over this many frames a return path that dropped even a
	// small fraction would leave hundreds of orphans, against a steady state of
	// at most the quoter's two live sides. A queue-depth check cannot see this:
	// the consumer keeps up with the producer, so the ring is empty at the end
	// whether or not anything was lost on the way back.
	EXPECT_LE(result.working_orders, 2U)
		<< "the gate is holding " << result.working_orders << " orders after "
		<< result.frames
		<< " frames; a ledger that grows with the frame count means outcomes "
		   "are not making it back, and the position limit would ratchet shut "
		   "over a long run";
	EXPECT_EQ(result.left_queued, 0U)
		<< "and the ring is empty, which is a quiescence check rather than a "
		   "loss check - see above for why the two are not the same";
}

TEST(AppLiveSessionRealtime, TheInferredSizeBoundsWhatWasFilled) {
	const realtime_outcome result =
		run_realtime(REALTIME_FRAMES, realtime_simulating());
	ASSERT_GT(result.injected, 0U);

	// crossing_fill_model documents injected_lots as an upper bound rather than
	// a count, and live there are two reasons for the slack: the book may hold
	// less than the model thought, and the ledger the model reads leads the
	// book for placements. A position *larger* than everything ever offered on
	// our behalf would mean an injection had been applied twice.
	EXPECT_LE(result.net, result.injected_lots)
		<< "we cannot be longer than the total size the model ever offered to "
		   "sell us";
	EXPECT_GE(result.injected_lots, 0);
}

TEST(AppLiveSessionRealtime, TheDefaultStillTradesNothingPassively) {
	const realtime_outcome result = run_realtime(REALTIME_FRAMES, {});

	EXPECT_EQ(result.injected, 0U);
	EXPECT_EQ(result.fills, 0U)
		<< "the production chain is unchanged under contention too, not just "
		   "when it is hand-driven";
	EXPECT_EQ(result.left_queued, 0U);
}

TEST(AppLiveSessionRealtime, TheInvariantsHoldAcrossManyInterleavings) {
	// One run exercises one schedule. This is the stress loop: same market,
	// same options, repeatedly, so a hand-off bug that needs an unlucky
	// interleaving has more than one chance to appear. Counts are deliberately
	// not compared between runs - under a real consumer they are allowed to
	// differ, and asserting they do not would be asserting the pipeline is
	// synchronous.
	for (int attempt = 0; attempt < REALTIME_RUNS; ++attempt) {
		const realtime_outcome result =
			run_realtime(REALTIME_FRAMES / 4, realtime_simulating());

		SCOPED_TRACE(attempt);
		EXPECT_EQ(result.left_queued, 0U);
		EXPECT_LE(result.net, result.injected_lots);
		EXPECT_EQ(result.takes, 0U);
	}
}

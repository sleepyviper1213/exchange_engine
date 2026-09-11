#include "replay.hpp"

#include "core/logging.hpp"
#include "core/util/slurp.hpp"
#include "market_data.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - the report's formatters
#include "seed.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/stopwatch.h>

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>


namespace exchange::app {

// --- replay: rebuild the venue's published depth from a JSONL diff capture ---
// The managed-local-order-book procedure end to end: seed from a REST snapshot,
// then stream diffs *through the sequencer*. The target is market data's
// l2_book throughout - the matching engine is not involved, because none of
// this is our order flow.
//
// --- why this goes through depth_reconstructor rather than applying directly -
//
// It used to decode the whole file with parse_binance_depth_updates and hand
// every frame to apply_depth_update in a loop. That is not a cheaper version of
// this; it is a different and wrong computation, in three ways.
//
// It applied frames the snapshot already covered. A capture is started before
// the snapshot is fetched - it has to be, or the frames bridging the two are
// lost - so the first frames on the file predate the seed. Their level sizes
// are absolute and older than the snapshot's, so replaying them on top wrote
// stale sizes over correct ones. A price touched only by those early frames
// then kept the stale value for the rest of the run. On a real 180s BTCUSDT
// capture that is 73 of 1799 frames, and nothing about the output said so.
//
// It could not see a gap. Absolute sizes mean a missing frame leaves no trace -
// the book simply, silently, stops matching the venue's - which is the entire
// reason depth_sequencer exists. A replay that cannot report a gap cannot tell
// you whether its own answer is meaningful.
//
// And it named Binance in the middle of an otherwise generic loop, which
// feed.hpp calls out by name as the thing the feed seam was built to remove.
// @see market_data/feed.hpp, market_data/reconstructor.hpp
int cmd_replay(const std::string &file, const std::string &snapshot_file,
			   int price_decimals, int qty_decimals) {
	using exchange::core::util::slurp;
	namespace md      = exchange::market_data;
	namespace binance = md::binance;

	// Optional seed: absolute levels from a saved REST snapshot, normalised so
	// the feed can hand it over as its first message.
	std::optional<md::book_snapshot> seed;
	// Read off before the move below empties the vectors, so the report can
	// state what it was seeded from without reaching into a moved-from object.
	md::sequence_t seed_sequence = 0;
	std::size_t seed_levels      = 0;
	if (!snapshot_file.empty()) {
		auto snapshot =
			load_seed_snapshot(snapshot_file, price_decimals, qty_decimals);
		if (!snapshot) return EXIT_FAILURE; // load_seed_snapshot said why
		seed_sequence = static_cast<md::sequence_t>(snapshot->lastUpdateId);
		seed_levels   = snapshot->bids.size() + snapshot->asks.size();
		seed          = binance::normalise(std::move(*snapshot));
	} else {
		// Worth saying plainly, and it is a stronger statement than it used to
		// be: with no seed the sequencer never leaves `awaiting_snapshot`, so
		// every event buffers and *nothing* is applied. The old loop produced a
		// partial book here; this produces an empty one, which is the honest
		// answer - a diff feed with no snapshot has no book in it.
		spdlog::warn("no --snapshot seed: every event will buffer and the book "
					 "will stay empty");
	}

	const std::string jsonl = slurp(file);
	if (jsonl.empty()) {
		spdlog::error("cannot read {} (missing or empty)", file);
		return EXIT_FAILURE;
	}
	spdlog::debug("read {} bytes from {}", jsonl.size(), file);

	// One feed, decoding lazily. The whole capture is never materialised and
	// simdjson's buffers amortise across it - which is also why the timing
	// below covers decode as well as apply.
	binance::jsonl_depth_feed feed =
		seed.has_value()
			? binance::jsonl_depth_feed(std::move(*seed),
										jsonl,
										price_decimals,
										qty_decimals)
			: binance::jsonl_depth_feed(jsonl, price_decimals, qty_decimals);
	md::depth_reconstructor replica;

	const spdlog::stopwatch watch;
	const md::feed_run run = md::drive(feed, replica);

	// --- the report --------------------------------------------------------
	// Printed, not logged, and that is not cosmetic. The ladder goes to stdout
	// because it is this command's *data* - something to pipe or redirect - and
	// the summary belongs with it. Splitting them across two streams put a
	// 128-line ladder in the middle of its own summary, because the default
	// logger is asynchronous: its lines are drained by a worker thread while
	// fmt::println writes straight through, so the interleaving was whatever
	// the scheduler happened to do.
	//
	// core::logging::flush() would not have fixed it. Under settings::async it
	// *enqueues* a flush rather than performing one, so the ordering would
	// still have been an accident that usually came out right - which is the
	// kind of correctness this tree refuses elsewhere for the same reason.
	// One stream, one order. spdlog keeps the diagnostics, which are genuinely
	// a different thing from the answer. @see cmd_backtest and cmd_trades,
	// which are shaped this way already.
	const md::sequencer_stats &stats = replica.stats();
	const bool clean = is_clean(run) && stats.gaps == 0 &&
					   feed.malformed() == 0 && replica.is_alive();

	fmt::println("replay {}  [{}]",
				 file,
				 clean ? "clean" : "SUSPECT - see the counters below");
	if (seed.has_value())
		fmt::println("  seed      {} ({} levels) from {}",
					 seed_sequence,
					 seed_levels,
					 snapshot_file);
	else fmt::println("  seed      none - nothing can be applied without one");
	fmt::println("  feed      {} events, {} snapshots, stopped: {}",
				 run.events,
				 run.snapshots,
				 run.stop);
	fmt::println("  sequence  {} applied, {} discarded, {} buffered, "
				 "{} overlapped, {} gaps",
				 stats.applied,
				 stats.discarded,
				 stats.buffered,
				 stats.overlapped,
				 stats.gaps);
	fmt::println("  pending   {} still held, {} dropped at the cap, "
				 "{} lines malformed",
				 replica.pending(),
				 replica.dropped(),
				 feed.malformed());
	fmt::println("  time      {:.4f}s to decode, sequence and apply", watch);

	// The counters above are the evidence; these say what to do about it, and
	// they go to the log because they are diagnostics rather than the answer.
	if (feed.malformed() != 0)
		spdlog::error("{} lines failed to decode; every one of them is a "
					  "sequence gap the replica had to be rebuilt from",
					  feed.malformed());
	if (stats.gaps != 0)
		spdlog::error("{} gaps: the capture is missing frames, so the book "
					  "below is whatever survived the last resync",
					  stats.gaps);
	if (stats.discarded != 0)
		spdlog::debug("{} events were already covered by the snapshot and were "
					  "discarded rather than applied on top of it",
					  stats.discarded);
	if (replica.crosses() != 0)
		spdlog::warn("{} events left the book crossed", replica.crosses());

	// Reported at error level and answered with a failing exit code, because
	// those two must agree: the command's job is to reconstruct a book, and
	// there is no book. That is a stronger test than `is_clean(run)`, which
	// only asks whether the file was read to the end - a capture can be read
	// perfectly and still leave no replica, which is exactly what happens with
	// no seed, or with a gap near the end that nothing resyncs from.
	if (!replica.is_alive()) {
		spdlog::error("the replica is not live at the end of the capture: "
					  "there is no book to print");
		if (!seed.has_value())
			spdlog::error("no snapshot was given, so nothing could ever have "
						  "been applied; pass --snapshot");
		return EXIT_FAILURE;
	}

	fmt::println("  replica   live at sequence {}", replica.last_sequence());
	fmt::println("{}",
				 md::book_ladder{.book           = &replica.book(),
								 .price_decimals = price_decimals,
								 .qty_decimals   = qty_decimals});

	// A gap is a fact about the recording, not a failure of this command, so it
	// is loud in the output and absent from the exit code. Failing to reach the
	// end of the file is the other way round. @see cmd_backtest, which draws
	// the line in the same place.
	if (!is_clean(run)) {
		spdlog::error("the capture did not replay to the end: {}", run.stop);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

} // namespace exchange::app

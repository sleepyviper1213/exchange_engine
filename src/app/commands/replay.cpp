#include "replay.hpp"

#include "core/logging.hpp"
#include "core/util/slurp.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<book_ladder>
#include "market_data.hpp"

#include <cstdlib>
#include <string>
#include <vector>

namespace exchange::app {

// --- replay: rebuild the venue's published depth from a JSONL diff capture ---
// The managed-local-order-book procedure end to end: seed from a REST snapshot,
// then stream diffs. The target is market data's l2_book throughout - the
// matching engine is not involved, because none of this is our order flow.
// @param snapshot_file  Non-empty to seed the book from a saved REST snapshot.
int cmd_replay(const std::string &file, const std::string &snapshot_file,
			   int price_decimals, int qty_decimals) {
	using exchange::core::util::slurp;
	namespace binance = market_data::binance;

	market_data::l2_book book;

	// Optional seed: absolute levels from a saved REST snapshot.
	if (!snapshot_file.empty()) {
		auto snap = binance::parse_binance_depth(slurp(snapshot_file),
												 price_decimals,
												 qty_decimals);
		if (!snap) {
			spdlog::error("snapshot parse failed for {}: {}",
						  snapshot_file,
						  snap.error());
			return EXIT_FAILURE;
		}
		spdlog::info("seeded from {}: {}", snapshot_file, *snap);
		// reset() rather than a set_level per level: it installs each side
		// wholesale through l2_book::load, which picks the best max_depth
		// levels straight into storage instead of paying a binary search and a
		// shift for every one of them. Logged before the move, because after it
		// there is nothing left to log.
		market_data::reset(book, binance::normalise(std::move(*snap)));
	} else {
		// Worth saying plainly: with no seed the book only ever holds the
		// prices the capture happened to touch, so its depth is an artefact of
		// the recording rather than the venue's published book.
		spdlog::warn("no --snapshot seed; the replayed book will be partial");
	}

	// Read + parse the JSONL feed (one depthUpdate frame per line).
	const std::string jsonl = slurp(file);
	if (jsonl.empty()) {
		spdlog::error("cannot read {} (missing or empty)", file);
		return EXIT_FAILURE;
	}
	spdlog::debug("read {} bytes from {}", jsonl.size(), file);
	const auto updates = binance::parse_binance_depth_updates(jsonl,
															  price_decimals,
															  qty_decimals);
	if (!updates) {
		spdlog::error("replay parse failed for {}: {}", file, updates.error());
		return EXIT_FAILURE;
	}

	// Apply every update to the book, timing the hot loop.
	const auto start   = std::chrono::steady_clock::now();
	std::size_t levels = 0;
	for (const auto &update : *updates) {
		binance::apply_depth_update(book, update);
		levels += update.bids.size() + update.asks.size();
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	const double secs  = std::chrono::duration<double>(elapsed).count();

	spdlog::info("replayed {} updates ({} level changes) from {} in {:.3f}s",
				 updates->size(),
				 levels,
				 file,
				 secs);
	fmt::println("{}",
				 market_data::book_ladder{.book           = &book,
										  .price_decimals = price_decimals,
										  .qty_decimals   = qty_decimals});
	return EXIT_SUCCESS;
}

} // namespace exchange::app

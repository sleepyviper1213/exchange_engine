#include "live.hpp"

#include "session/live_feed.hpp"
#include "core/logging.hpp"
#include "market_data/binance/depth_speed.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - fmt::formatter<book_ladder>
#include "market_data/reconstructor.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <fmt/format.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace exchange::app {

// The feed half of the live pipeline; this command drives no session.
using session::live_feed_options;
using session::live_feed_report;
using session::live_handler;
using session::run_live_feed;

int cmd_live(const std::string &symbol, int seconds, std::string_view speed,
			 int limit, int price_decimals, int qty_decimals, int depth) {
	namespace asio    = boost::asio;
	namespace binance = market_data::binance;

	if (seconds < 0) {
		spdlog::error("seconds must not be negative (got {})", seconds);
		return EXIT_FAILURE;
	}

	// The enum owns the list of cadences, so an unrecognised --speed is
	// reported here rather than trusted from the CLI's own copy of it.
	const auto cadence = binance::from_string(speed);
	if (!cadence) {
		spdlog::error("unknown speed \"{}\": want {} or {}",
					  speed,
					  binance::depth_speed::every_100ms,
					  binance::depth_speed::every_1000ms);
		return EXIT_FAILURE;
	}

	live_feed_options options;
	options.duration       = std::chrono::seconds(seconds);
	options.limit          = limit;
	options.price_decimals = price_decimals;
	options.qty_decimals   = qty_decimals;
	options.speed          = *cadence;
	// The replica, and the whole of the state this command owns. Declared
	// before the io_context and destroyed after it, which is more than
	// run_live_feed asks for: the pipeline coroutine is the replica's only
	// writer, and nothing it spawns holds a reference to this. The detached
	// snapshot fetches talk to a shared_ptr channel instead, so one still
	// running when the run ends cannot reach anything that has gone away.
	market_data::depth_reconstructor replica;
	static_assert(live_handler<market_data::depth_reconstructor>,
				  "the reconstructor is the pipeline's handler as written");

	asio::io_context ioc;
	live_feed_report report;
	std::string failure;

	// One thread, and that is a requirement rather than a simplification: the
	// frame chain and the snapshot chain both mutate `replica` without a lock,
	// and what makes that sound is that they cannot be running at once.
	// @see app/live_feed.hpp
	asio::co_spawn(ioc,
				   run_live_feed(symbol, &replica, options),
				   [&](const std::exception_ptr &ep, live_feed_report r) {
					   if (ep) {
						   try {
							   std::rethrow_exception(ep);
						   } catch (const std::exception &e) {
							   failure = e.what();
						   }
					   } else {
						   report = std::move(r);
					   }
				   });
	ioc.run();

	if (!failure.empty()) {
		spdlog::error("live feed threw: {}", failure);
		return EXIT_FAILURE;
	}

	spdlog::info("live feed stopped: {}", report.stopped);
	spdlog::info("{} frames ({} malformed), {} reconnects, snapshots {}/{} "
				 "applied",
				 report.frames,
				 report.malformed,
				 report.reconnects,
				 report.snapshots_applied,
				 report.snapshots_requested);
	spdlog::info("replica {}: {}",
				 replica.is_alive() ? "live" : "dead",
				 replica.stats());

	// The book, on stdout and unadorned, for the same reason every other
	// command puts its result there: something downstream may be reading it.
	fmt::println(
		"{}",
		market_data::book_ladder{
			.book           = &replica.book(),
			.price_decimals = price_decimals,
			.qty_decimals   = qty_decimals,
			.max_levels     = static_cast<std::size_t>(std::min(depth, 0))});

	// A run that never brought the replica live told us nothing about the
	// venue, whatever else it did - and it is the one outcome the counters
	// above do not make obvious, because a feed that streamed thousands of
	// frames into a book that was never seeded looks busy and is empty.
	if (!replica.is_alive()) {
		spdlog::error("the replica is not live: {} events are still buffered "
					  "and no snapshot bridged them",
					  replica.pending());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

} // namespace exchange::app

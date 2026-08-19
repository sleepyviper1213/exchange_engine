#include "snapshot.hpp"

#include "core/logging.hpp"
#include "core/util/slurp.hpp"
#include "market-data/format.hpp" // IWYU pragma: keep - fmt::formatter<book_ladder>
#include "market_data.hpp"
#include "transport.hpp"

#include <fmt/chrono.h> // IWYU pragma: keep - fmt::formatter<std::chrono::duration>

#include <chrono>
#include <cstdlib>
#include <expected>
#include <string>

namespace exchange::app {

int cmd_snapshot(const std::string &symbol, const std::string &file, int limit,
				 int price_decimals, int qty_decimals) {
	namespace binance = market_data::binance;

	const auto begin = std::chrono::system_clock::now();
	std::expected<std::string, std::string> json = std::unexpected("uninit");

	if (!file.empty()) {
		spdlog::info("loading depth snapshot from {}", file);
		json = exchange::core::util::slurp(file);
		if (json->empty())
			json = std::unexpected(fmt::format("cannot read {}", file));
	} else {
		auto [host, target] = binance::depth_snapshot(symbol, limit);
		spdlog::info("fetching depth snapshot {} limit={} from {}",
					 symbol,
					 limit,
					 host);
		spdlog::debug("GET {}{}", host, target);
		json =
			exchange::transport::rest::get(std::move(host), std::move(target));
	}

	if (!json) {
		spdlog::error("snapshot fetch failed: {}", json.error());
		return EXIT_FAILURE;
	}
	spdlog::debug("snapshot payload {} bytes", json->size());

	auto snapshot =
		binance::parse_binance_depth(*json, price_decimals, qty_decimals);
	if (!snapshot) {
		spdlog::error("snapshot parse failed: {}", snapshot.error());
		return EXIT_FAILURE;
	}
	const auto end = std::chrono::system_clock::now();
	spdlog::info("snapshot ready in {}: {}", end - begin, *snapshot);

	// A REST snapshot is published depth, so it reconstructs into an l2_book -
	// resting anonymous orders in a matching engine would model a queue the
	// payload says nothing about.
	//
	// reset() rather than a set_level per level: it installs each side
	// wholesale through l2_book::load, which picks the best max_depth levels
	// straight into storage instead of paying a binary search and a shift for
	// every one. The book is reported before the move, because after it there
	// is nothing left to report.
	market_data::l2_book book;
	market_data::reset(book, binance::normalise(std::move(*snapshot)));

	// The result, on stdout, untimestamped: this is what a caller redirecting
	// stdout is asking for. book_ladder rather than the book directly, because
	// the command was given the symbol's tick and step and they are the only
	// thing that turns the book's scaled integers back into quoted prices.
	fmt::println("{}",
				 market_data::book_ladder{&book, price_decimals, qty_decimals});
	return EXIT_SUCCESS;
}

} // namespace exchange::app

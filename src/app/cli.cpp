#include "cli.hpp"

#include "core/concurrency/affinity.hpp"
#include "market_data.hpp"
#include "trading-engine.hpp"
#include "transport.hpp"
#include "core/util/slurp.hpp"

#include <CLI/CLI.hpp>
#include <fmt/std.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace exchange::engine;

// Per-command drivers — thin wrappers over transport (I/O) and the engine.
// File-internal: the CLI is the only caller (see run_cli below).
namespace exchange::app {
int cmd_snapshot(const std::string &symbol, const std::string &file, int limit,
                 int price_decimals, int qty_decimals) {
	namespace binance = market_data::binance;

	const auto begin = std::chrono::system_clock::now();
	std::expected<std::string, std::string> json = std::unexpected("uninit");

	if (!file.empty()) {
		json = exchange::core::util::slurp(file.c_str());
		if (json->empty())
			json = std::unexpected(fmt::format("cannot read {}", file));
	} else {
		json = exchange::transport::rest::get(
			"api.binance.com",
			fmt::format("/api/v3/depth?symbol={}&limit={}", symbol, limit));
	}

	if (!json) {
		fmt::println(stderr, "fetch error: {}", json.error());
		return EXIT_FAILURE;
	}

	const auto snapshot =
		binance::parse_binance_depth(*json, price_decimals, qty_decimals);
	if (!snapshot) {
		fmt::println(stderr, "parse error: {}", binance::message(snapshot.error()));
		return EXIT_FAILURE;
	}

	order_book book;
	for (const auto &[price, volume] : snapshot->bids)
		book.add_order(Side::BID, price, volume);
	for (const auto &[price, volume] : snapshot->asks)
		book.add_order(Side::ASK, price, volume);
	const auto end = std::chrono::system_clock::now();

	fmt::println("Elapsed: {}  lastUpdateId={}  bids={}  asks={}",
	             end - begin,
	             snapshot->lastUpdateId,
	             snapshot->bids.size(),
	             snapshot->asks.size());
	const auto bid = book.best_bid();
	const auto ask = book.best_ask();
	if (bid && ask)
		fmt::println("best bid={}  best ask={}  spread={} ticks",
		             *bid,
		             *ask,
		             *ask - *bid);
	return EXIT_SUCCESS;
}

int cmd_capture(std::string symbol, const std::string &outfile, int seconds,
                std::string_view speed) {
	if (seconds <= 0) {
		fmt::println(stderr, "seconds must be positive");
		return EXIT_FAILURE;
	}

	// Binance stream names are lowercase.
	std::ranges::transform(symbol,
	                       symbol.begin(),
	                       [](unsigned char c) {
		                       return static_cast<char>(std::tolower(c));
	                       });

	// @depth pushes every 1000ms; @depth@100ms every 100ms (Binance spot).
	const std::string stream =
		speed == "1000ms" ? symbol + "@depth" : symbol + "@depth@100ms";

	const auto result = exchange::transport::ws::capture("stream.binance.com",
		"9443",
		"/ws/" + stream,
		outfile,
		std::chrono::seconds(seconds));
	if (!result) {
		fmt::println(stderr, "capture error: {}", result.error());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int cmd_demo(std::uint64_t num_orders) {
	namespace affinity = core::concurrency::affinity;

	constexpr Price kMid = 10000; // reference price the synthetic flow orbits
	if (num_orders == 0) {
		fmt::println(stderr, "num_orders must be positive");
		return EXIT_FAILURE;
	}

	// Place the producer (this thread) and consumer on dedicated cores, each on
	// its own physical core where the topology allows — the SPSC hand-off pays
	// real cross-core coherency traffic instead of thrashing one core's L1/L2.
	affinity::CoreAllocator cores(affinity::discover());
	const auto producer_core = cores.reserve("producer");
	const auto consumer_core = cores.reserve("consumer");
	const auto core_str      = [](std::optional<affinity::CoreId> c) {
		return c ? fmt::to_string(*c) : "any";
	};
	fmt::println("topology: {} logical CPUs, {} physical cores{}  "
	             "(producer->cpu {}, consumer->cpu {})",
	             cores.topology().logical_cpus,
	             cores.topology().physical_cores,
	             cores.topology().smt ? ", SMT" : "",
	             core_str(producer_core),
	             core_str(consumer_core));

	// The i-th order: sides alternate, prices sweep +/-5 ticks around the mid
	// so opposing orders cross.
	const auto make_order = [kMid](std::uint64_t i) noexcept {
		const Side side     = (i & 1U) ? Side::BID : Side::ASK;
		const Price price   = kMid + static_cast<Price>(i % 11U) - 5U;
		const Volume volume = 1 + static_cast<Volume>(i % 5U);
		return event::Command::place(Order{.id = i + 1U,
		                                   .side = side,
		                                   .price = price,
		                                   .volume = volume});
	};

	std::atomic<std::uint64_t> trade_count{0};
	std::atomic<std::int64_t> matched_volume{0};

	execution::MatchingEngine<1024> engine([&](const std::vector<Trade> &batch) noexcept {
		std::int64_t v = 0;
		for (const Trade &t : batch) v += t.volume;
		trade_count.fetch_add(batch.size(), std::memory_order_relaxed);
		matched_volume.fetch_add(v, std::memory_order_relaxed);
	});

	const auto start = std::chrono::steady_clock::now();

	// Consumer: drain until every submitted command has been applied.
	std::thread consumer([&] {
		static_cast<void>(cores.pin_this_thread_to("consumer"));
		std::uint64_t applied = 0;
		while (applied < num_orders) {
			const std::size_t n = engine.drain();
			if (n == 0) std::this_thread::yield();
			else applied += n;
		}
	});

	// Producer: this thread. Retries on a full lockfree (lossless
	// back-pressure).
	static_cast<void>(cores.pin_this_thread_to("producer"));
	for (std::uint64_t i = 0; i < num_orders; ++i) {
		const event::Command cmd = make_order(i);
		while (!engine.submit(cmd)) std::this_thread::yield();
	}

	consumer.join();
	const auto elapsed = std::chrono::steady_clock::now() - start;
	const double secs  = std::chrono::duration<double>(elapsed).count();

	fmt::println("submitted {} orders in {:.3f}s  ({:.2f}M orders/s)",
	             num_orders,
	             secs,
	             static_cast<double>(num_orders) / secs / 1e6);
	fmt::println("trades: {}   matched volume: {}",
	             trade_count.load(),
	             matched_volume.load());
	const auto bid = engine.book().best_bid();
	const auto ask = engine.book().best_ask();
	fmt::println("resting book — best bid: {}   best ask: {}",
	             bid ? fmt::to_string(*bid) : "none",
	             ask ? fmt::to_string(*ask) : "none");
	return EXIT_SUCCESS;
}

// --- replay: apply a JSONL diff-depth capture to an OrderBook ----------------
// @param snapshot_file  Non-empty to seed the book from a saved REST snapshot.
int cmd_replay(const std::string &file, const std::string &snapshot_file,
               int price_decimals, int qty_decimals) {
	using namespace exchange::engine;
	using exchange::core::util::slurp;
	namespace binance = market_data::binance;

	order_book book;

	// Optional seed: absolute levels from a saved REST snapshot.
	if (!snapshot_file.empty()) {
		const auto snap =
			binance::parse_binance_depth(slurp(snapshot_file.c_str()),
			                             price_decimals,
			                             qty_decimals);
		if (!snap) {
			fmt::println(stderr, "snapshot parse error: {}", binance::message(snap.error()));
			return EXIT_FAILURE;
		}
		for (const auto &[price, volume] : snap->bids)
			book.set_level(Side::BID, price, volume);
		for (const auto &[price, volume] : snap->asks)
			book.set_level(Side::ASK, price, volume);
	}

	// Read + parse the JSONL feed (one depthUpdate frame per line).
	const std::string jsonl = slurp(file.c_str());
	if (jsonl.empty()) {
		fmt::println(stderr, "cannot read {}", file);
		return EXIT_FAILURE;
	}
	const auto updates = binance::parse_binance_depth_updates(jsonl,
		price_decimals,
		qty_decimals);
	if (!updates) {
		fmt::println(stderr, "replay parse error: {}", binance::message(updates.error()));
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

	fmt::println("replayed {} updates ({} level changes) from {} in {:.3f}s",
	             updates->size(),
	             levels,
	             file,
	             secs);
	const auto bid = book.best_bid();
	const auto ask = book.best_ask();
	if (bid && ask)
		fmt::println("final book — best bid={}  best ask={}  spread={} ticks",
		             *bid,
		             *ask,
		             *ask - *bid);
	return EXIT_SUCCESS;
}

// Each registrar binds CLI11 options to storage that must outlive the call —
// parsing runs later, back in main() — so the option variables are function
// static. The CLI is built and parsed exactly once, so that is safe.
void add_snapshot(CLI::App &app, int &rc) {
	auto *snap = app.add_subcommand(
		"snapshot",
		"Fetch (or load) a Binance depth snapshot and print top of book");
	static std::string symbol, file;
	static int limit = 100, price_decimals = 2, qty_decimals = 2;
	snap->add_option("symbol", symbol, "Binance symbol, e.g. SOLUSDT");
	snap->add_option("--file",
	                 file,
	                 "Load a saved depth JSON or fetching live");
	snap->add_option("--limit", limit, "REST depth limit")
		->capture_default_str();
	snap->add_option("--price-decimals", price_decimals, "Tick precision")
		->capture_default_str();
	snap->add_option("--qty-decimals", qty_decimals, "Step precision")
		->capture_default_str();
	snap->callback([&rc] {
		if (symbol.empty() && file.empty())
			throw CLI::ValidationError("snapshot", "provide SYMBOL or --file");
		rc = cmd_snapshot(symbol, file, limit, price_decimals, qty_decimals);
	});
}

void add_capture(CLI::App &app, int &rc) {
	auto *cap = app.add_subcommand(
		"capture",
		"Stream a Binance diff-depth WebSocket to a JSONL file");
	static std::string symbol, outfile, speed = "100ms";
	static int seconds                        = 30;
	cap->add_option("symbol", symbol, "Binance symbol")->required();
	cap->add_option("outfile", outfile, "Destination JSONL file")->required();
	cap->add_option("--seconds", seconds, "Recording duration (seconds)")
		->capture_default_str();
	cap->add_option("--speed", speed, "Update cadence")
		->capture_default_str()
		->check(CLI::IsMember({"100ms", "1000ms"}));
	cap->callback([&rc] { rc = cmd_capture(symbol, outfile, seconds, speed); });
}

void add_demo(CLI::App &app, int &rc) {
	auto *demo = app.add_subcommand(
		"demo",
		"Run the MatchingEngine end-to-end over the SPSC queue");
	static std::uint64_t num_orders = 2'000'000;
	demo->add_option("num_orders", num_orders, "Synthetic orders to submit")
		->capture_default_str();
	demo->callback([&rc] { rc = cmd_demo(num_orders); });
}

void add_replay(CLI::App &app, int &rc) {
	auto *replay = app.add_subcommand(
		"replay",
		"Replay a JSONL diff-depth capture through an OrderBook");
	static std::string file, snapshot;
	static int price_decimals = 2, qty_decimals = 2;
	replay->add_option("file", file, "JSONL capture of depthUpdate frames")
		->required()
		->check(CLI::ExistingFile);
	replay
		->add_option("--snapshot",
		             snapshot,
		             "Seed the book from a saved REST depth JSON")
		->check(CLI::ExistingFile);
	replay->add_option("--price-decimals", price_decimals, "Tick precision")
		->capture_default_str();
	replay->add_option("--qty-decimals", qty_decimals, "Step precision")
		->capture_default_str();
	replay->callback([&rc] {
		rc = cmd_replay(file, snapshot, price_decimals, qty_decimals);
	});
}
} // namespace cli
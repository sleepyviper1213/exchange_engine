#include "cli.hpp"

#include "core/concurrency/affinity.hpp"
#include "core/concurrency/affinity/format.hpp"
#include "core/logging.hpp"
#include "core/util/slurp.hpp"
#include "market-data/format.hpp"
#include "market_data.hpp"
#include "trading-engine.hpp"
#include "trading-engine/format.hpp"
#include "transport.hpp"

#include <CLI/CLI.hpp>
#include <fmt/std.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;

// Per-command drivers — thin wrappers over transport (I/O) and the engine.
// File-internal: the CLI is the only caller (see run_cli below).
//
// The split here is between a command's RESULT and its COMMENTARY. A book
// ladder, a snapshot, a throughput figure go to stdout through fmt::println,
// unadorned, because something downstream may be reading them — timestamping
// those would corrupt data, not annotate it. Everything else — what was
// fetched, how long it took, what went wrong — goes to the log, which is stderr
// plus the file (see core/logging.hpp).
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

	const auto snapshot =
		binance::parse_binance_depth(*json, price_decimals, qty_decimals);
	if (!snapshot) {
		spdlog::error("snapshot parse failed: {}", snapshot.error());
		return EXIT_FAILURE;
	}

	// A REST snapshot is published depth, so it reconstructs into an l2_book —
	// resting anonymous orders in a matching engine would model a queue the
	// payload says nothing about.
	market_data::l2_book book;
	for (const auto &[price, qty] : snapshot->bids)
		book.set_level(side_t::bid, price, qty);
	for (const auto &[price, qty] : snapshot->asks)
		book.set_level(side_t::ask, price, qty);
	const auto end = std::chrono::system_clock::now();
	spdlog::info("snapshot ready in {}: {}", end - begin, *snapshot);

	// The result, on stdout, untimestamped: this is what a caller redirecting
	// stdout is asking for. book_ladder rather than the book directly, because
	// the command was given the symbol's tick and step and they are the only
	// thing that turns the book's scaled integers back into quoted prices.
	fmt::println("{}",
				 market_data::book_ladder{&book, price_decimals, qty_decimals});
	return EXIT_SUCCESS;
}

// Capture the venue's published diff-depth feed: the `<symbol>@depth` stream
// whose frames carry absolute aggregate sizes per price. market-data decides
// which endpoint that is; transport just records the frames.
int cmd_capture(const std::string &symbol, const std::string &outfile,
				int seconds, std::string_view speed) {
	namespace binance = market_data::binance;

	if (seconds <= 0) {
		spdlog::error("seconds must be positive (got {})", seconds);
		return EXIT_FAILURE;
	}

	auto [host, port, target] = binance::diff_depth_stream(
		symbol,
		speed == "1000ms" ? binance::depth_speed::every_1000ms
						  : binance::depth_speed::every_100ms);

	spdlog::info("capturing {} @{} for {}s from {} -> {}",
				 symbol,
				 speed,
				 seconds,
				 host,
				 outfile);

	const auto result =
		exchange::transport::ws::capture(std::move(host),
										 std::move(port),
										 std::move(target),
										 outfile,
										 std::chrono::seconds(seconds));
	if (!result) {
		spdlog::error("capture failed: {}", result.error());
		return EXIT_FAILURE;
	}
	spdlog::info("capture complete: {}", outfile);
	return EXIT_SUCCESS;
}

int cmd_demo(std::uint64_t num_orders) {
	namespace affinity = core::concurrency::affinity;

	constexpr price_t MID =
		10000; // reference price_t the synthetic flow orbits
	if (num_orders == 0) {
		spdlog::error("num_orders must be positive");
		return EXIT_FAILURE;
	}

	// Place the producer (this thread) and consumer on dedicated cores, each on
	// its own physical core where the topology allows — the SPSC hand-off pays
	// real cross-core coherency traffic instead of thrashing one core's L1/L2.
	affinity::core_allocator cores(affinity::discover());
	const auto producer_core = cores.reserve("producer");
	const auto consumer_core = cores.reserve("consumer");
	const auto core_str      = [](std::optional<affinity::core_id> c) {
		return c ? fmt::to_string(*c) : std::string("any");
	};
	// Pinning is a property of the run, not a result of it: an unpinned pair
	// measures the scheduler, so which cores were reserved has to be
	// recoverable from the log when a throughput figure later looks wrong.
	spdlog::info("{}  (producer->cpu {}, consumer->cpu {})",
				 cores.get_topology(),
				 core_str(producer_core),
				 core_str(consumer_core));
	if (!producer_core || !consumer_core)
		spdlog::warn("a core reservation was refused; the hand-off may share a "
					 "core and the throughput below is not comparable");

	// One listing, so the routing this demo exercises is trivial — but the
	// commands still have to name it, because a partition refuses a symbol it
	// was not given rather than inventing a book for it.
	constexpr symbol_id_t SYMBOL = 0;

	// The i-th order: sides alternate, prices sweep +/-5 ticks around the mid
	// so opposing orders cross.
	const auto make_order = [MID](std::uint64_t i) noexcept {
		// Locals are deliberately not named after their types: inside a scope
		// that declares a `price`, `static_cast<price>` resolves to the
		// variable rather than the type and stops compiling.
		const side_t s       = (i & 1U) ? side_t::bid : side_t::ask;
		const price_t px     = MID + static_cast<price_t>(i % 11U) - 5U;
		const quantity_t qty = 1 + static_cast<quantity_t>(i % 5U);
		return event::command::place(order{.id        = i + 1U,
										   .symbol_id = SYMBOL,
										   .side      = s,
										   .price     = px,
										   .qty       = qty});
	};

	std::atomic<std::uint64_t> trade_count{0};
	std::atomic<std::int64_t> matched_volume{0};

	execution::engine_partition<1024> engine(
		[&](const std::vector<Trade> &batch) noexcept {
			std::int64_t v = 0;
			for (const Trade &t : batch) v += t.volume;
			trade_count.fetch_add(batch.size(), std::memory_order_relaxed);
			matched_volume.fetch_add(v, std::memory_order_relaxed);
		});
	// On the consumer's side of the contract, and before the producer starts.
	engine.listing(SYMBOL);

	const auto start = std::chrono::steady_clock::now();

	// Consumer: drain until every submitted command has been applied.
	std::thread consumer([&] {
		// pin_this_thread_to has already logged which syscall refused and on
		// what core. What it cannot know is what that costs *here*, which is
		// the only thing worth adding: an unpinned consumer makes the figure
		// below a measurement of the scheduler as much as of the engine.
		if (!cores.pin_this_thread_to("consumer"))
			spdlog::warn("consumer is unpinned; the throughput below is not "
						 "comparable with a pinned run");
		std::uint64_t applied = 0;
		while (applied < num_orders) {
			const std::size_t n = engine.drain_and_flush();
			if (n == 0) std::this_thread::yield();
			else applied += n;
		}
	});

	// Producer: this thread. Retries on a full lockfree (lossless
	// back-pressure).
	if (!cores.pin_this_thread_to("producer"))
		spdlog::warn("producer is unpinned; the throughput below is not "
					 "comparable with a pinned run");
	for (std::uint64_t i = 0; i < num_orders; ++i) {
		const event::command cmd = make_order(i);
		while (!engine.submit(cmd)) std::this_thread::yield();
	}

	consumer.join();
	const auto elapsed = std::chrono::steady_clock::now() - start;
	const double secs  = std::chrono::duration<double>(elapsed).count();

	fmt::println("submitted {} orders in {:.3f}s  ({:.2f}M orders/s)",
				 num_orders,
				 secs,
				 static_cast<double>(num_orders) / secs / 1e6);
	fmt::println("trades: {}   matched qty: {}",
				 trade_count.load(),
				 matched_volume.load());
	fmt::println("resting {}", *engine.book(SYMBOL));
	return EXIT_SUCCESS;
}

// --- replay: rebuild the venue's published depth from a JSONL diff capture ---
// The managed-local-order-book procedure end to end: seed from a REST snapshot,
// then stream diffs. The target is market data's l2_book throughout — the
// matching engine is not involved, because none of this is our order flow.
// @param snapshot_file  Non-empty to seed the book from a saved REST snapshot.
int cmd_replay(const std::string &file, const std::string &snapshot_file,
			   int price_decimals, int qty_decimals) {
	using exchange::core::util::slurp;
	namespace binance = market_data::binance;

	market_data::l2_book book;

	// Optional seed: absolute levels from a saved REST snapshot.
	if (!snapshot_file.empty()) {
		const auto snap = binance::parse_binance_depth(slurp(snapshot_file),
													   price_decimals,
													   qty_decimals);
		if (!snap) {
			spdlog::error("snapshot parse failed for {}: {}",
						  snapshot_file,
						  snap.error());
			return EXIT_FAILURE;
		}
		for (const auto &[price, qty] : snap->bids)
			book.set_level(side_t::bid, price, qty);
		for (const auto &[price, qty] : snap->asks)
			book.set_level(side_t::ask, price, qty);
		spdlog::info("seeded from {}: {}", snapshot_file, *snap);
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
				 market_data::book_ladder{&book, price_decimals, qty_decimals});
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
	static int seconds = 30;
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
} // namespace exchange::app
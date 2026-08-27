#include "demo.hpp"

#include "core/chrono/wall.hpp"
#include "core/concurrency/affinity.hpp"
#include "core/concurrency/affinity/format.hpp" // IWYU pragma: keep - fmt::formatter<topology>
#include "core/logging.hpp"
#include "core/metrics.hpp"
#include "core/metrics/format.hpp" // IWYU pragma: keep - fmt::formatter<registry>, <histogram::snapshot>
#include "core/util/owned_file.hpp"
#include "event/lifecycle/lifecycle.hpp"
#include "execution.hpp"
#include "format.hpp" // IWYU pragma: keep - fmt::formatter<order_book>, <order_manager>, <startup>, <shutdown>

#include <fmt/std.h> // IWYU pragma: keep - fmt::formatter<std::filesystem::path>
#include <spdlog/stopwatch.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace exchange::engine;
using namespace exchange::engine::orders;

namespace exchange::app {
namespace {} // namespace

int cmd_demo(std::uint64_t num_orders,
			 const core::metrics::settings &metrics_settings) {
	namespace affinity  = core::concurrency::affinity;
	namespace metrics   = core::metrics;
	namespace lifecycle = event::lifecycle;

	constexpr price_t MID =
		10000; // reference price_t the synthetic flow orbits
	if (num_orders == 0) {
		spdlog::error("num_orders must be positive");
		return EXIT_FAILURE;
	}

	// Place the producer (this thread) and consumer on dedicated cores, each on
	// its own physical core where the topology allows - the SPSC hand-off pays
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

	// One listing, so the routing this demo exercises is trivial - but the
	// commands still have to name it, because a partition refuses a symbol it
	// was not given rather than inventing a book for it.
	constexpr symbol_id_t SYMBOL = 0;

	// Self-cancelling crossing pairs: an ASK rests at a price, then a BID at
	// the same price and size consumes it whole. Sides still alternate and the
	// price still sweeps +/-5 ticks around the mid, so the run exercises match,
	// rest, pop_front and level insert/erase at varied sorted positions - but
	// the book returns to empty after every pair.
	//
	// That last property is the point, and it used to be missing. Pairing each
	// order with an opposite one only *near* it in price left the extremes
	// uncrossed, so resting liquidity accumulated without bound; the book
	// absorbed it (its node pool chains another block) but order_manager will
	// not, because refusing an order beats forgetting a live one. The run
	// therefore filled the record store at ~186k orders and rejected every
	// command after that, reporting a throughput figure that was really
	// measuring how fast the engine can say no. benchmark/matching_engine.cpp
	// generates its flow this way for the same reason.
	const auto make_order = [MID](std::uint64_t i) noexcept {
		// Locals are deliberately not named after their types: inside a scope
		// that declares a `price`, `static_cast<price>` resolves to the
		// variable rather than the type and stops compiling.
		const std::uint64_t pair = i / 2U;
		const side_t s           = (i & 1U) ? side_t::bid : side_t::ask;
		const price_t px         = MID + static_cast<price_t>(pair % 11U) - 5U;
		const quantity_t qty     = 1 + static_cast<quantity_t>(pair % 5U);
		return event::command::place(order{.id        = i + 1U,
										   .symbol_id = SYMBOL,
										   .side      = s,
										   .price     = px,
										   .qty       = qty});
	};

	std::atomic<std::uint64_t> trade_count{0};
	std::atomic<std::int64_t> matched_volume{0};
	std::atomic<std::uint64_t> reject_count{0};
	// The two totals the session's shutdown record is built from. Counted
	// rather than inferred: `applied` must come out equal to num_orders and
	// every order must produce at least one outcome, and a record that stated
	// those instead of measuring them would agree with the run by construction
	// - which is the one thing a figure meant to be checked against a replay
	// must not do.
	std::atomic<std::uint64_t> applied_count{0};
	std::atomic<std::uint64_t> outcome_count{0};
	// The reason of the first refusal, kept so the summary can name it. One
	// cause explains a whole run's worth of rejections here - the interesting
	// question is never "which of these many reasons" but "why did it start".
	std::atomic<reject_reason> first_reject{reject_reason::NONE};

	// Declared unconditionally (it is four cache lines on the stack, nothing
	// more) but only wired into the partition - and so only ever written to -
	// when the operator asked for it. See core/metrics/settings.hpp: metrics
	// are off by default, and a caller that never mentions --metrics-enabled
	// gets exactly the cost of an unmetered partition.
	execution::partition_metrics engine_metrics{
		// The settings are plain integers because that is what an INI file and
		// a
		// command line hold; the conversion into durations happens here, once,
		// which is the only place both spellings are in scope.
		.drain_latency_ns{metrics::latency_budgets{
			.p99 =
				std::chrono::nanoseconds{metrics_settings.drain_p99_budget_ns},
			.p999 =
				std::chrono::nanoseconds{metrics_settings.drain_p999_budget_ns},
			.max =
				std::chrono::nanoseconds{metrics_settings.drain_max_budget_ns},
		}},
	};

	// Watches drain_latency_ns on metrics_settings.interval_ms for the whole
	// run rather than only at the end - see core/metrics/sla_monitor.hpp.
	// std::optional so it is constructed only when metrics were asked for,
	// and reset() right after the run so the monitor's thread is not still
	// polling a histogram this function is about to let go out of scope.
	std::optional<metrics::sla_monitor> drain_monitor;
	if (metrics_settings.enabled)
		drain_monitor.emplace(
			engine_metrics.drain_latency_ns,
			std::chrono::milliseconds(metrics_settings.interval_ms),
			[](const metrics::histogram &h) {
				spdlog::warn("drain latency breached its budget: {}", h.read());
			});

	execution::engine_partition<1024> engine(
		[&](const std::vector<trade> &batch) noexcept {
			std::int64_t v = 0;
			for (const trade &t : batch) v += t.volume;
			trade_count.fetch_add(batch.size(), std::memory_order_relaxed);
			matched_volume.fetch_add(v, std::memory_order_relaxed);
		},
		// An outcome sink, and not decoration: without one a run in which the
		// engine refused every order looks exactly like a fast one. It reports
		// fewer trades and a *higher* orders/s, because saying no is cheaper
		// than matching. That is the most misleading way for a benchmark to
		// fail, so the refusals are counted and printed.
		[&](const std::vector<order_outcome> &batch) noexcept {
			outcome_count.fetch_add(batch.size(), std::memory_order_relaxed);
			std::uint64_t refused = 0;
			for (const order_outcome &o : batch) {
				if (o.type != OutcomeType::REJECTED) continue;
				++refused;
				reject_reason none = reject_reason::NONE;
				// Relaxed: only the first writer matters and nothing is ordered
				// against it - the value is read after both threads have
				// joined.
				first_reject.compare_exchange_strong(none,
													 o.reason,
													 std::memory_order_relaxed,
													 std::memory_order_relaxed);
			}
			if (refused != 0)
				reject_count.fetch_add(refused, std::memory_order_relaxed);
		},
		execution::book_manager::DEFAULT_BOOK_CAPACITY,
		execution::order_manager::DEFAULT_CAPACITY,
		metrics_settings.enabled ? &engine_metrics : nullptr);
	// On the consumer's side of the contract, and before the producer starts.
	engine.listing(SYMBOL);

	// The session opens here, which is exactly the moment the record describes:
	// the books exist and nothing has been submitted yet. Its id is the same
	// wall-clock reading that stamps it - lifecycle/fwd.hpp suggests precisely
	// that, and it is enough, because the only question ever asked of a session
	// id is whether it differs from the last one's.
	//
	// COLD, and not a placeholder: this run starts from empty books with no
	// journal behind it, so the order ids below mean nothing outside it. That
	// is the fact a reader of a log needs before it can interpret a single
	// command.
	// The id is the reading, and why that is enough is now stated where the
	// contract is rather than here. @see lifecycle::session_of
	const auto opened_at = core::chrono::wall_now();
	const lifecycle::startup opened{.session = lifecycle::session_of(opened_at),
									.timestamp = opened_at,
									.mode      = lifecycle::StartMode::COLD};
	// Commentary, not result: a session boundary annotates the run rather than
	// being data something downstream parses off stdout, so it goes to the log
	// like the topology and the pinning do. When the journal of TODO.md #6
	// exists, this is also the first record it appends.
	spdlog::info("{}", opened);

	const spdlog::stopwatch watch;

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
		// Relaxed, and safe without more: the join below is the happens-before
		// edge that publishes this to the thread that reads it.
		applied_count.store(applied, std::memory_order_relaxed);
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

	// And the session closes. CLEAN even in a run that refused orders, which is
	// not a technicality: StopReason says how much of the session to believe,
	// and a refusal is the venue answering - the books, the counts and the log
	// are all exactly what they say they are. HALTED would mean the engine
	// stopped with work still queued, which cannot happen here because the
	// consumer drains until every submitted command has been applied; FAULT
	// would mean the in-memory state is not to be trusted. Neither is this run.
	const lifecycle::shutdown closed{
		.session          = opened.session,
		.timestamp        = core::chrono::wall_now(),
		.reason           = lifecycle::StopReason::CLEAN,
		.commands_applied = applied_count.load(std::memory_order_relaxed),
		.events_published = trade_count.load(std::memory_order_relaxed) +
							outcome_count.load(std::memory_order_relaxed)};
	spdlog::info("{}", closed);

	const double secs = watch.elapsed().count();

	fmt::println("submitted {} orders in {:.3f}s  ({:.2f}M orders/s)",
				 num_orders,
				 secs,
				 static_cast<double>(num_orders) / secs / 1e6);
	fmt::println("trades: {}   matched qty: {}",
				 trade_count.load(),
				 matched_volume.load());
	fmt::println("resting {}", *engine.book(SYMBOL));

	// The record store's own reading, printed every run rather than only on
	// trouble: peak against capacity is the number the sizing has to be argued
	// from, and this is the only place it can be read.
	fmt::println("{}", engine.orders());

	// Reference wiring for core/metrics: name the fields recorded above, print
	// the drain-latency distribution the way docs/performance.md asks any
	// latency budget be read (a percentile, not a mean), and - if the operator
	// asked for it - overwrite the exposition file a scrape-based collector
	// would tail. drain_monitor already watched this continuously while the
	// run was in flight; this is the final read after it stopped, covering
	// whatever happened between its last tick and the run ending.
	if (metrics_settings.enabled) {
		metrics::registry registry;
		registry.add("engine_commands_processed",
					 engine_metrics.commands_processed);
		registry.add("engine_trades_emitted", engine_metrics.trades_emitted);
		registry.add("engine_misroutes", engine_metrics.misroutes);
		registry.add("engine_drain_latency_ns",
					 engine_metrics.drain_latency_ns);

		fmt::println("drain latency: {}",
					 engine_metrics.drain_latency_ns.read());

		// One last, synchronous check for the gap between drain_monitor's
		// last periodic tick and now, reusing its own callback instead of
		// hand-rolling the same is_healthy()-then-warn a second time - see
		// core/metrics/sla_monitor.hpp::check_now().
		drain_monitor->check_now();
		// Stop watching now that the run is over and this function is about
		// to let drain_latency_ns' owner (engine_metrics) go out of scope.
		drain_monitor.reset();

		// fmt::print onto a FILE*, which is the good half of each of the two
		// obvious spellings and neither of their bad ones. Formatting goes
		// straight into the file - `out << fmt::format("{}", registry)` would
		// materialise the whole exposition as a std::string and then copy it,
		// which is the reason to want fmt here at all. And the open still
		// reports failure as a *value*, where fmt::output_file reports it by
		// throwing: writing this file is the last and least important thing the
		// command does, and it must be able to fail without unwinding a run
		// whose real work has already finished and been reported.
		//
		// "wb" truncates, and the handle owns itself - see
		// core/util/owned_file.hpp on both the mode strings and why the
		// ownership is in the type rather than spelled out here.
		const core::util::owned_file out =
			core::util::open_shared(metrics_settings.output_file, "wb");
		if (out == nullptr) {
			spdlog::error("could not open metrics file {}",
						  metrics_settings.output_file);
		} else {
			fmt::print(out.get(), "{}", registry);
			spdlog::info("wrote metrics to {}", metrics_settings.output_file);
		}
	}

	const std::uint64_t refused = reject_count.load();
	if (refused == 0) return EXIT_SUCCESS;

	// A refused order never reached a book, so it is missing from the trade
	// count and from the orders/s above - both of which are then measuring a
	// smaller run than the one that was asked for. Loud, and a failure exit:
	// a throughput figure taken from a partial run is worse than none.
	spdlog::error(
		"{} of {} orders were refused ({}); the figures above describe "
		"the {} that were not",
		refused,
		num_orders,
		describe(first_reject.load()),
		num_orders - refused);
	return EXIT_FAILURE;
}

} // namespace exchange::app

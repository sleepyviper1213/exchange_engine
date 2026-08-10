// Per-operation latency of experimental::cached_optimised_order_book.
//
// The question this file exists to answer is a p99, not a mean, and Google
// Benchmark cannot answer it: its aggregates are over *repetitions* of a whole
// timed loop, so a single slow update disappears into the average of the
// millions around it. docs/performance.md asks for p50/p99/p99.9 on every
// target, which means timing individual calls.
//
// At ~10 ns per update that is right at the edge of what a clock can resolve,
// so the harness reads the CPU cycle counter directly, characterises its own
// overhead, and subtracts it. See sample_latency() for what that costs and
// what it cannot fix.

#include "trading-engine/order_book/cached_optimised_order_book.hpp"

#include "core/concurrency/affinity/affinity.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#define EXCHANGE_HAS_CYCLE_CLOCK 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#elif defined(__aarch64__)
#define EXCHANGE_HAS_CYCLE_CLOCK 1
#else
#define EXCHANGE_HAS_CYCLE_CLOCK 0
#endif

namespace affinity = exchange::core::concurrency::affinity;

using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::experimental::cached_optimised_order_book;

namespace {

/// @brief One update in a pre-built stream.
///
/// @c sample separates the operation under test from the one that puts the
/// book back where the next repetition needs it. Measuring an insert means
/// erasing what you just inserted, and the erase is a different path with a
/// different cost — without this flag the two average together and neither
/// number is the one being asked for.
struct op {
	side_t side;
	price_t price;
	quantity_t quantity;
	bool sample;
};

/// @brief Fill both sides to capacity around @p mid, so the book is at the
///        steady state a live feed keeps it in rather than half empty.
template <std::size_t N>
void fill(cached_optimised_order_book<N> &book, price_t mid) {
	for (std::size_t i = 0; i < N; ++i) {
		book.update_level(side_t::bid, mid - 1 - i, 100);
		book.update_level(side_t::ask, mid + 1 + i, 100);
	}
}

/// @brief Pin this thread and raise its priority, via core's affinity layer.
///
/// docs/performance.md: "Pin threads and state the topology. An unpinned run
/// measures the scheduler." Without this the tail is dominated by migrations
/// and preemptions rather than by the code — the first run of this file showed
/// maxima of 90 us to 1 ms against a p50 of 30 ns, which is the OS, not the
/// book. It does not make the machine quiet; it only removes the two sources
/// of tail noise a benchmark can remove by itself.
///
/// Both calls are best-effort by contract, and the priority one needs an
/// elevated process on Windows. The result is reported in the @c pinned
/// counter so a run that silently failed to pin is not read as a clean one.
///
/// @return true only if both the pin and the priority took effect.
bool pin_and_prioritise(affinity::core_id core) {
	const bool pinned = affinity::pin_this_thread(core);
	const bool raised =
		affinity::set_this_thread_priority(affinity::thread_priority::high);
	return pinned && raised;
}

#if EXCHANGE_HAS_CYCLE_CLOCK

/// @brief Open a timed region: fence, then read the counter.
///
/// @c lfence first so the read cannot drift above whatever preceded it; plain
/// @c rdtsc after, because the fence has already done the ordering that
/// @c rdtscp would repeat. Pairing this with cycle_stop() rather than using a
/// symmetric serialising read at both ends is what keeps the floor near 5 ns
/// instead of 15 — at a p50 under 30 ns the clock is a third of the result, so
/// the asymmetry is worth the extra function.
[[nodiscard]] inline std::uint64_t cycle_start() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	_mm_lfence();
	return __rdtsc();
#else
	std::uint64_t stamp = 0;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(stamp)::"memory");
	return stamp;
#endif
}

/// @brief Close a timed region.
///
/// @c rdtscp does not retire until every prior instruction has, so it captures
/// the work under test without needing a fence in front of it.
[[nodiscard]] inline std::uint64_t cycle_stop() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	unsigned aux = 0;
	return __rdtscp(&aux);
#else
	std::uint64_t stamp = 0;
	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(stamp)::"memory");
	return stamp;
#endif
}

/// @brief Cycle-counter ticks per nanosecond, measured against steady_clock.
///
/// The x86 TSC is invariant — it counts at a fixed rate regardless of the
/// core's current frequency — so this converts ticks to wall-clock nanoseconds,
/// not to core cycles. A number here is what a user waits, which is the thing
/// a 30 ns budget is denominated in.
[[nodiscard]] double ticks_per_ns() {
	using clock           = std::chrono::steady_clock;
	const auto wall_start = clock::now();
	const auto tick_start = cycle_start();

	// Long enough that steady_clock's own resolution is noise, short enough
	// not to stall the suite.
	while (clock::now() - wall_start < std::chrono::milliseconds(200))
		benchmark::DoNotOptimize(cycle_stop());

	const auto tick_end = cycle_stop();
	const auto wall_end = clock::now();
	const auto elapsed_ns =
		std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end -
															 wall_start)
			.count();
	return static_cast<double>(tick_end - tick_start) /
		   static_cast<double>(elapsed_ns);
}

/// @brief The cost of the measurement itself, in ticks.
///
/// Two back-to-back cycle_now() calls with nothing between them. Subtracting
/// the median of this from every sample removes the harness from the result;
/// it cannot remove the *variance* the harness adds, which is why the p99.9
/// column is reported but not trusted as tightly as p50 and p99.
[[nodiscard]] std::uint64_t clock_overhead_ticks() {
	std::vector<std::uint64_t> samples;
	samples.reserve(20000);
	for (int i = 0; i < 20000; ++i) {
		const auto start = cycle_start();
		const auto stop  = cycle_stop();
		samples.push_back(stop - start);
	}
	std::ranges::sort(samples);
	return samples[samples.size() / 2];
}

/// @brief p50 / p99 / p99.9 of @p samples, in nanoseconds, with the harness
///        overhead already removed.
void report_percentiles(benchmark::State &state,
						std::vector<std::uint64_t> &samples,
						std::uint64_t overhead, double per_ns) {
	if (samples.empty()) return;
	std::ranges::sort(samples);

	const auto quantile = [&](double q) {
		const auto index = static_cast<std::size_t>(
			q * static_cast<double>(samples.size() - 1));
		const auto ticks = samples[index];
		// Saturate rather than wrap: a sample can land below the median
		// overhead purely by measurement jitter.
		const auto net = ticks > overhead ? ticks - overhead : 0;
		return static_cast<double>(net) / per_ns;
	};

	state.counters["p50_ns"]  = quantile(0.50);
	state.counters["p99_ns"]  = quantile(0.99);
	state.counters["p999_ns"] = quantile(0.999);
	state.counters["max_ns"]  = quantile(1.0);
	state.counters["samples"] = static_cast<double>(samples.size());
}

/// @brief Apply @p stream to @p book, timing only the ops marked for sampling.
///
/// @param state The benchmark state; percentiles land in its counters.
/// @param book The book under test, already at steady state.
/// @param stream The updates to apply, cycled through as needed. Pre-built so
///        no generation cost lands inside the timed pair.
template <std::size_t N>
void sample_latency(benchmark::State &state,
					cached_optimised_order_book<N> &book,
					const std::vector<op> &stream) {
	static const bool pinned      = pin_and_prioritise(2);
	static const double per_ns    = ticks_per_ns();
	static const std::uint64_t oh = clock_overhead_ticks();

	std::vector<std::uint64_t> samples;
	samples.reserve(1u << 20);

	// Mask, not modulo. `% stream.size()` is a 64-bit hardware divide — 20-30
	// cycles, comfortably more than the update it is supposed to be indexing —
	// and it lands between the two clock reads. Every stream built in this file
	// is a power of two so the mask is exact.
	const std::size_t wrap = stream.size() - 1;
	std::size_t cursor     = 0;
	for (auto _ : state) {
		const auto &entry = stream[cursor];
		cursor            = (cursor + 1) & wrap;

		if (!entry.sample) {
			// Off the measurement, but still on Google Benchmark's clock: the
			// reported Time column is therefore a per-stream-op average, not a
			// per-sample one. The percentile counters are what this file is
			// for; read Time only as a sanity check.
			book.update_level(entry.side, entry.price, entry.quantity);
			benchmark::DoNotOptimize(&book);
			continue;
		}

		const auto start = cycle_start();
		book.update_level(entry.side, entry.price, entry.quantity);
		const auto stop = cycle_stop();

		benchmark::DoNotOptimize(&book);
		if (samples.size() < samples.capacity())
			samples.push_back(stop - start);
	}

	report_percentiles(state, samples, oh, per_ns);
	state.counters["depth"] = static_cast<double>(N);
	// The noise floor, published so the columns above can be read against it.
	// A p50 within a few ns of this is measuring the clock, not the book.
	state.counters["clock_ns"] = static_cast<double>(oh) / per_ns;
	state.counters["pinned"]   = pinned ? 1 : 0;
}

// --------------------------------------------------------------------------
// The paths, separately: they have genuinely different costs and averaging
// them together would hide the one that matters.
// --------------------------------------------------------------------------

/// Overwrite a resting level's size. No shift — a search plus a store, and the
/// overwhelming majority of what an L2 diff feed carries.
template <std::size_t N>
void BM_UpdateOverwrite(benchmark::State &state) {
	constexpr price_t mid = 1'000'000;
	cached_optimised_order_book<N> book;
	fill(book, mid);

	// Spread the writes across the whole side so the search is not always
	// hitting the same line.
	std::vector<op> stream;
	std::mt19937_64 rng(42);
	for (std::size_t i = 0; i < 1024; ++i) {
		const bool bid   = (rng() & 1u) != 0;
		const auto slot  = rng() % N;
		const auto price = bid ? mid - 1 - slot : mid + 1 + slot;
		stream.emplace_back(bid ? side_t::bid : side_t::ask,
							price,
							static_cast<quantity_t>(100 + (rng() % 900)),
							true);
	}

	sample_latency(state, book, stream);
}

/// Overwrite the touch specifically — the single hottest level on a live feed,
/// and the one whose line is certain to be resident.
template <std::size_t N>
void BM_UpdateOverwriteTouch(benchmark::State &state) {
	constexpr price_t mid = 1'000'000;
	cached_optimised_order_book<N> book;
	fill(book, mid);

	std::vector<op> stream;
	stream.reserve(1024);
	for (std::size_t i = 0; i < 1024; ++i) {
		stream.emplace_back(side_t::bid,
							mid - 1,
							static_cast<quantity_t>(100 + i),
							true);
	}

	sample_latency(state, book, stream);
}

/// Insert a new price at the touch on a full side: the maximum-shift case, and
/// exactly where a diff feed concentrates its inserts. Every resting level
/// moves and the worst one is evicted.
///
/// @c mid is above every resting bid, so each insert lands at index 0. The
/// erase that follows is what makes the next insert an insert again rather
/// than an overwrite, and it is excluded from the samples — it is the other
/// path, benchmarked separately below.
template <std::size_t N>
void BM_InsertAtTouch(benchmark::State &state) {
	constexpr price_t mid = 1'000'000;
	cached_optimised_order_book<N> book;
	fill(book, mid);

	std::vector<op> stream;
	for (std::size_t i = 0; i < 512; ++i) {
		stream.push_back({side_t::bid, mid, quantity_t{100}, true});
		stream.push_back({side_t::bid, mid, quantity_t{0}, false});
	}

	sample_latency(state, book, stream);
}

/// Erase the touch and let the side close up: the mirror of the insert shift.
/// Same stream as the insert case with the flags swapped.
template <std::size_t N>
void BM_EraseAtTouch(benchmark::State &state) {
	constexpr price_t mid = 1'000'000;
	cached_optimised_order_book<N> book;
	fill(book, mid);

	std::vector<op> stream;
	for (std::size_t i = 0; i < 512; ++i) {
		stream.push_back({side_t::bid, mid, quantity_t{100}, false});
		stream.push_back({side_t::bid, mid, quantity_t{0}, true});
	}

	sample_latency(state, book, stream);
}

/// A feed-shaped mix: mostly overwrites near the touch, some inserts and
/// removals. The number to quote for "what does an update cost", since the
/// isolated paths above are the bracketing extremes rather than the workload.
template <std::size_t N>
void BM_UpdateFeedMix(benchmark::State &state) {
	constexpr price_t mid = 1'000'000;
	cached_optimised_order_book<N> book;
	fill(book, mid);

	std::vector<op> stream;
	std::mt19937_64 rng(1337);
	for (std::size_t i = 0; i < 4096; ++i) {
		const bool bid = (rng() & 1u) != 0;
		// Depth-weighted toward the touch, which is where a diff feed puts
		// most of its traffic.
		const auto slot  = std::min<std::uint64_t>(rng() % N, rng() % N);
		const auto price = bid ? mid - 1 - slot : mid + 1 + slot;

		const auto roll = rng() % 100;
		const auto quantity =
			roll < 5 ? quantity_t{0} // 5% removals
					 : static_cast<quantity_t>(100 + (rng() % 900));
		stream.emplace_back(bid ? side_t::bid : side_t::ask,
							price,
							quantity,
							true);
	}

	sample_latency(state, book, stream);
}

// Depths: 8 fits a side in 8 cache lines, 128 is l2_book's default, 1024 is
// the "keep everything" end where the shift dominates.
BENCHMARK(BM_UpdateOverwrite<8>);
BENCHMARK(BM_UpdateOverwrite<128>);
BENCHMARK(BM_UpdateOverwrite<1024>);

BENCHMARK(BM_UpdateOverwriteTouch<8>);
BENCHMARK(BM_UpdateOverwriteTouch<128>);
BENCHMARK(BM_UpdateOverwriteTouch<1024>);

BENCHMARK(BM_InsertAtTouch<8>);
BENCHMARK(BM_InsertAtTouch<128>);
BENCHMARK(BM_InsertAtTouch<1024>);

BENCHMARK(BM_EraseAtTouch<8>);
BENCHMARK(BM_EraseAtTouch<128>);
BENCHMARK(BM_EraseAtTouch<1024>);

BENCHMARK(BM_UpdateFeedMix<8>);
BENCHMARK(BM_UpdateFeedMix<128>);
BENCHMARK(BM_UpdateFeedMix<1024>);

#endif // EXCHANGE_HAS_CYCLE_CLOCK

// --------------------------------------------------------------------------
// Throughput, for the platforms without a cycle counter and as a cross-check
// that the sampled numbers above are not an artefact of the harness.
// --------------------------------------------------------------------------

template <std::size_t N>
void BM_UpdateThroughput(benchmark::State &state) {
	constexpr price_t mid = 1'000'000;
	cached_optimised_order_book<N> book;
	for (std::size_t i = 0; i < N; ++i) {
		book.update_level(side_t::bid, mid - 1 - i, 100);
		book.update_level(side_t::ask, mid + 1 + i, 100);
	}

	std::vector<op> stream;
	std::mt19937_64 rng(1337);
	for (std::size_t i = 0; i < 4096; ++i) {
		const bool bid   = (rng() & 1u) != 0;
		const auto slot  = std::min<std::uint64_t>(rng() % N, rng() % N);
		const auto price = bid ? mid - 1 - slot : mid + 1 + slot;
		stream.emplace_back(bid ? side_t::bid : side_t::ask,
							price,
							static_cast<quantity_t>(100 + (rng() % 900)),
							true);
	}

	const std::size_t wrap = stream.size() - 1; // power of two; see above
	std::size_t cursor     = 0;
	for (auto _ : state) {
		const auto &entry = stream[cursor];
		cursor            = (cursor + 1) & wrap;
		book.update_level(entry.side, entry.price, entry.quantity);
		benchmark::DoNotOptimize(&book);
	}
	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_UpdateThroughput<8>);
BENCHMARK(BM_UpdateThroughput<128>);
BENCHMARK(BM_UpdateThroughput<1024>);

} // namespace

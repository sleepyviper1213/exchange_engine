#include "market-data/binance/binance_depth.hpp"
#include "market-data/l2_book.hpp"
#include "trading-engine/order_book/order_book.hpp"
#include "replay_data.hpp"

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <string_view>


using namespace exchange::engine;

using namespace exchange::market_data;

// Real-world market replay: seed an OrderBook from a Binance REST depth
// snapshot, then stream a sequence of `depthUpdate` diff events through it —
// the managed local-order-book procedure Binance documents for the
// `<symbol>@depth` feed. Each diff level is an *absolute* aggregated size (0 =
// remove). The feed is offline and
// deterministic so the timed region has no network or JSON cost (see
// replay_data.hpp for the input).

namespace {
/**
 * @brief Steady-state replay throughput: apply the diff feed to a warm book.
 *
 * Because diffs set absolute sizes, re-running the same feed keeps the book
 * bounded, so this isolates pure update throughput (levels/sec) from book
 * construction.
 * @param state Google Benchmark state.
 */
void BM_MarketReplay_SteadyState(benchmark::State &state) {
	const auto [snap, feed, levels] = replay::load();

	order_book book;
	replay::seed_book(book, snap);

	for (auto _ : state) {
		for (const auto &u : feed) replay::apply_ob(book, u);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
	state.SetLabel(fmt::format("{} events / {} levels", feed.size(), levels));
}

/**
 * @brief Steady-state replay into the cache-optimised l2_book — the A/B partner
 *        of BM_MarketReplay_SteadyState.
 *
 * Identical feed and absolute-set_level semantics, but the book is a flat,
 * price-sorted {price, qty} array per side instead of order_book's per-level
 * heap FIFO of Orders. The gap between the two is the reconstruction cache win:
 * l2_book's set_level is a binary search plus an in-place qty write over
 * contiguous memory, with no per-level allocation or pointer chase.
 * @param state Google Benchmark state.
 */
void BM_MarketReplay_L2Book(benchmark::State &state) {
	const auto [snap, feed, levels] = replay::load();

	l2_book book;
	replay::seed_l2(book, snap);

	for (auto _ : state) {
		for (const auto &u : feed) replay::apply_l2(book, u);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
	state.SetLabel(fmt::format("{} events / {} levels (l2_book, cache-optimised)",
							   feed.size(),
							   levels));
}

/**
 * @brief Cold end-to-end replay: rebuild the book from the snapshot and replay
 *        the whole feed each iteration (seed-plus-replay latency).
 * @param state Google Benchmark state.
 */
void BM_MarketReplay_Cold(benchmark::State &state) {
	const auto [snap, feed, levels] = replay::load();

	for (auto _ : state) {
		order_book book;
		replay::seed_book(book, snap);
		for (const auto &u : feed) replay::apply_ob(book, u);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
}

/**
 * @brief Steady-state replay that PARSES each raw depthUpdate JSON frame with a
 *        reused DepthParser before applying it — the real tick-to-book path.
 *
 * One DepthParser drives every frame through apply_update, which reuses
 * simdjson's structural-index/tape buffers and the input buffer across frames
 * (and skips the per-frame level vectors). This is the steady @c \@depth feed
 * cost with the parser reused, as intended in production. Compare its ns/level
 * against BM_MarketReplay_ParseOneShot to read off what reuse buys, and against
 * BM_MarketReplay_SteadyState to separate parse cost from pure apply.
 * @param state Google Benchmark state.
 */
void BM_MarketReplay_ParseReused(benchmark::State &state) {
	const auto data = replay::load_raw();

	l2_book book;
	replay::seed_l2(book, data.snap);
	binance::DepthParser parser; // reused across every frame and iteration

	for (auto _ : state) {
		for (const std::string_view frame : data.feed.frames) {
			auto meta = parser.apply_update(book,
											frame,
											data.price_decimals,
											data.qty_decimals);
			benchmark::DoNotOptimize(meta);
		}
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(data.levels));
	state.SetBytesProcessed(state.iterations() *
							static_cast<std::int64_t>(data.feed.bytes.size()));
	state.SetLabel(fmt::format("{} frames / {} levels (reused parser)",
							   data.feed.frames.size(),
							   data.levels));
}

/**
 * @brief The same parse-and-apply path, but constructs a fresh parser per frame
 *        via the one-shot free function — the buffer-amortization baseline.
 *
 * apply_binance_depth_update builds a new simdjson parser and input buffer on
 * every call, so the gap to BM_MarketReplay_ParseReused is exactly the cost of
 * not reusing the parser across the feed.
 * @param state Google Benchmark state.
 */
void BM_MarketReplay_ParseOneShot(benchmark::State &state) {
	const auto data = replay::load_raw();

	l2_book book;
	replay::seed_l2(book, data.snap);

	for (auto _ : state) {
		for (const std::string_view frame : data.feed.frames) {
			auto meta = binance::apply_binance_depth_update(book,
															frame,
															data.price_decimals,
															data.qty_decimals);
			benchmark::DoNotOptimize(meta);
		}
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(data.levels));
	state.SetBytesProcessed(state.iterations() *
							static_cast<std::int64_t>(data.feed.bytes.size()));
	state.SetLabel(fmt::format("{} frames / {} levels (one-shot parser)",
							   data.feed.frames.size(),
							   data.levels));
}

BENCHMARK(BM_MarketReplay_SteadyState)
->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarketReplay_L2Book)
->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarketReplay_Cold)
->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarketReplay_ParseReused)
->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarketReplay_ParseOneShot)
->Unit(benchmark::kMicrosecond);
} // namespace

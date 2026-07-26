#include "market-data/binance/binance_depth.hpp"
#include "trading-engine/order_book/order_book.hpp"
#include "replay_data.hpp"

#include <benchmark/benchmark.h>
#include <fmt/format.h>


using namespace exchange::engine;

using namespace exchange::market_data;

// Real-world market replay: seed an OrderBook from a Binance REST depth
// snapshot, then stream a sequence of `depthUpdate` diff events through it —
// the managed local-order-book procedure Binance documents for the
// `<symbol>@depth` feed. Each diff level is an *absolute* aggregated size (0 =
// remove), applied via binance::apply_depth_update. The feed is offline and
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
		for (const auto &u : feed) binance::apply_depth_update(book, u);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
	state.SetLabel(fmt::format("{} events / {} levels", feed.size(), levels));
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
		for (const auto &u : feed) binance::apply_depth_update(book, u);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
}

BENCHMARK(BM_MarketReplay_SteadyState)
->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarketReplay_Cold)
->Unit(benchmark::kMicrosecond);
} // namespace

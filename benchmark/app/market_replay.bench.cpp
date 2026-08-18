#include "market-data/binance/binance_depth.hpp"
#include "market-data/l2_book.hpp"
#include "market-data/replay.fixture.hpp"
#include "trading-engine/order_book/order_book.hpp"

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <string_view>


using namespace exchange::engine;

using namespace exchange::market_data;

// Real-world market replay: seed an OrderBook from a Binance REST depth
// snapshot, then stream a sequence of `depthUpdate` diff events through it -
// the managed local-order-book procedure Binance documents for the
// `<symbol>@depth` feed. Each diff level is an *absolute* aggregated size (0 =
// remove). The feed is offline and
// deterministic so the timed region has no network or JSON cost (see
// market-data/replay.fixture.hpp for the input).
//
// This lives under app/ for the same reason depth_feed_bridge does: it is the
// only benchmark that names both subsystems, running the venue's L2 feed
// against market-data's l2_book and the engine's order_book side by side.

namespace {

using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;

/**
 * @brief Apply one absolute L2 size to an order_book - the A/B baseline's shim.
 *
 * @c order_book has no @c set_level of its own, on purpose: an L2 diff carries
 * no order identity, so an absolute-size primitive on the matching book can
 * only rest synthetic orders with invented FIFO position that @c cancel_order
 * cannot see. What it does expose is the honest way to reach the same aggregate
 * through the public order-by-order API - read the level, then top it up or
 * drain it - and that is exactly the work an L2-onto-L3 mapping would have to
 * do. Measuring it here keeps the comparison alive without the primitive
 * existing in the shipped book.
 *
 * The mapping stays in this file rather than in the shared fixture: market-data
 * deliberately offers no such function, so the conflation the subsystem split
 * exists to prevent belongs where it is visibly a measurement, at the one join
 * that is allowed to name both sides.
 *
 * @note A raise appends a FIFO node rather than collapsing the level onto one,
 *       so this costs a touch more than the old @c order_book::set_level did.
 *       That is the point: the collapse was only cheap because it discarded the
 *       identity the L3 book exists to keep.
 */
void set_level_ob(order_book &book, side_t side, price_t price,
				  quantity_t target) {
	const quantity_t resting = book.volume_at_price(price, side);
	if (target > resting) book.add_order(side, price, target - resting);
	else if (target < resting) book.delete_order(side, price, resting - target);
}

/**
 * @brief Seed @p book with a snapshot's levels via absolute L2 sizes.
 * @param book Book to populate (assumed empty).
 * @param snap Snapshot whose bid/ask levels are inserted.
 */
void seed_book(order_book &book, const binance::DepthSnapshot &snap) {
	for (const auto &[price, qty] : snap.bids)
		set_level_ob(book, side_t::bid, price, qty);
	for (const auto &[price, qty] : snap.asks)
		set_level_ob(book, side_t::ask, price, qty);
}

/// @brief Apply one diff event to an order_book - the A/B baseline only.
/// @see set_level_ob for why the mapping goes through the public API.
void apply_ob(order_book &book, const binance::DepthUpdate &update) {
	for (const auto &[price, qty] : update.bids)
		set_level_ob(book, side_t::bid, price, qty);
	for (const auto &[price, qty] : update.asks)
		set_level_ob(book, side_t::ask, price, qty);
}

/**
 * @brief Steady-state replay throughput: apply the diff feed to a warm book.
 *
 * Because diffs set absolute sizes, re-running the same feed keeps the book
 * bounded, so this isolates pure update throughput (levels/sec) from book
 * construction.
 */
void BM_MarketReplay_SteadyState(benchmark::State &state) {
	const auto [snap, feed, levels] = replay::load();

	order_book book;
	seed_book(book, snap);

	for (auto _ : state) {
		for (const auto &u : feed) apply_ob(book, u);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
	state.SetLabel(fmt::format("{} events / {} levels", feed.size(), levels));
}

/**
 * @brief Steady-state replay into the cache-optimised l2_book - the A/B partner
 *        of BM_MarketReplay_SteadyState.
 *
 * Identical feed and absolute-set_level semantics, but the book is a flat,
 * price-sorted {price, qty} array per side instead of order_book's per-level
 * heap FIFO of Orders. The gap between the two is the reconstruction cache win:
 * l2_book's set_level is a binary search plus an in-place qty write over
 * contiguous memory, with no per-level allocation or pointer chase.
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
	state.SetLabel(
		fmt::format("{} events / {} levels (l2_book, cache-optimised)",
					feed.size(),
					levels));
}

/**
 * @brief Cold end-to-end replay: rebuild the book from the snapshot and replay
 *        the whole feed each iteration (seed-plus-replay latency).
 */
void BM_MarketReplay_Cold(benchmark::State &state) {
	const auto [snap, feed, levels] = replay::load();

	for (auto _ : state) {
		order_book book;
		seed_book(book, snap);
		for (const auto &u : feed) apply_ob(book, u);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
}

/**
 * @brief Steady-state replay that PARSES each raw depthUpdate JSON frame with a
 *        reused DepthParser before applying it - the real tick-to-book path.
 *
 * One DepthParser drives every frame through apply_update, which reuses
 * simdjson's structural-index/tape buffers and the input buffer across frames
 * (and skips the per-frame level vectors). This is the steady @c \@depth feed
 * cost with the parser reused, as intended in production. Compare its ns/level
 * against BM_MarketReplay_ParseOneShot to read off what reuse buys, and against
 * BM_MarketReplay_SteadyState to separate parse cost from pure apply.
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
 *        via the one-shot free function - the buffer-amortization baseline.
 *
 * apply_binance_depth_update builds a new simdjson parser and input buffer on
 * every call, so the gap to BM_MarketReplay_ParseReused is exactly the cost of
 * not reusing the parser across the feed.
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

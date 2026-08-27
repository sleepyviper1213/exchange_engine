#include "order_book/order_book.hpp"

#include "order_book/trade.hpp"

#include <benchmark/benchmark.h>

#include <random>
#include <vector>


using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::engine::allocation_policy;
using exchange::engine::order_book;

// Pre-generate a reproducible stream of prices so RNG cost is not timed.
namespace {
std::vector<price_t> makePrices(std::size_t n) {
	std::mt19937_64 rng(42);
	std::uniform_int_distribution<price_t> dist(1, 1'000'000);
	std::vector<price_t> prices;
	prices.reserve(n);
	for (std::size_t i = 0; i < n; ++i) prices.push_back(dist(rng));
	return prices;
}

// Insert N orders into an empty book. Measures lower_bound + sorted insert.
//
// One book, reset between iterations, rather than a fresh one per iteration.
// Constructing a book takes the order pool's block and both ladders' level
// cells up front, and destroying it walks every level returning its nodes -
// roughly a millisecond either way, which at the low end of the range is a
// thousand times the N inserts. Inside the timed region that constant was the
// measurement: the same insert path appeared to speed up 400x from N=8 to
// N=8192 on nothing but division.
//
// Leaving one book to fill across iterations is not the alternative - the same
// N prices go in every time, so the orders would pile up until the pool ran out
// and add_order started failing rather than resting. clear() is what makes the
// reset cheap enough to do per iteration: it returns the cells but keeps the
// blocks, so every iteration starts from the same warm, empty book.
void BM_AddOrder(benchmark::State &state) {
	const auto n      = static_cast<std::size_t>(state.range(0));
	const auto prices = makePrices(n);

	order_book book;

	for (auto _ : state) {
		for (auto price : prices) book.add_order(side_t::bid, price, 10);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();

		// Off the clock: emptying the book is what the next iteration needs,
		// not part of resting an order. The loop opens timed and closes timed,
		// so there is no second Pause to collide with this one.
		state.PauseTiming();
		book.clear();
		state.ResumeTiming();
	}
	state.SetItemsProcessed(state.iterations() * n);
}

BENCHMARK(BM_AddOrder)
->RangeMultiplier(16)->Range(8, 8 << 10);

// Query best prices on a pre-filled book (pure read path).
void BM_GetBestPrices(benchmark::State &state) {
	const auto prices = makePrices(static_cast<std::size_t>(state.range(0)));
	order_book book;
	for (const auto price : prices) {
		book.add_order(side_t::bid, price, 10);
		book.add_order(side_t::ask, price, 10);
	}

	for (auto _ : state) {
		auto best = book.best_bid();
		benchmark::DoNotOptimize(best);
	}
}

BENCHMARK(BM_GetBestPrices)
->RangeMultiplier(8)->Range(8, 8 << 10);

// One *partial* sweep of a single deep level, under each allocation policy -
// the only case where the two do different work, since a sweep that clears a
// level fills every order on it either way.
//
// The shapes differ, and both are worth having a number for. Price-time walks
// only the orders it consumes, popping each in turn: half the level here, and
// nothing behind it is touched. Pro-rata walks the level twice - once to total
// the floored shares and learn the residual, once to allocate - and touches
// every order at the price, filling most of them partially rather than
// retiring them. So pro-rata is a constant factor above price-time for the same
// traded quantity, and the constant is what this measures.
void BM_CrossOneLevel(benchmark::State &state, allocation_policy policy) {
	const auto resting        = static_cast<std::size_t>(state.range(0));
	constexpr quantity_t LOTS = 10;
	// Half the level's aggregate, so the aggressor never clears it.
	const auto sweep =
		static_cast<quantity_t>(resting * static_cast<std::size_t>(LOTS) / 2);

	order_book book{1U << 15, policy};
	std::vector<exchange::engine::trade> trades;
	trades.reserve(resting);

	for (auto _ : state) {
		// Off the clock, as in BM_AddOrder: rebuilding the level is what the
		// next iteration needs, not part of crossing one. clear() keeps the
		// pool blocks, so every iteration starts from the same warm book.
		state.PauseTiming();
		book.clear();
		trades.clear();
		for (std::size_t i = 0; i < resting; ++i)
			book.add_order(side_t::ask, 100, LOTS);
		state.ResumeTiming();

		book.place_order(
			{.id = 1, .side = side_t::bid, .price = 100, .qty = sweep},
			trades);
		benchmark::DoNotOptimize(trades.data());
		benchmark::ClobberMemory();
	}
	// Orders resting at the level, not trades printed: it is the level's depth
	// that both policies scale with, and comparing them per-order is the point.
	state.SetItemsProcessed(state.iterations() * resting);
}

BENCHMARK_CAPTURE(BM_CrossOneLevel, price_time, allocation_policy::PRICE_TIME)
	->RangeMultiplier(8)
	->Range(8, 8 << 7);
BENCHMARK_CAPTURE(BM_CrossOneLevel, pro_rata, allocation_policy::PRO_RATA)
	->RangeMultiplier(8)
	->Range(8, 8 << 7);
} // namespace

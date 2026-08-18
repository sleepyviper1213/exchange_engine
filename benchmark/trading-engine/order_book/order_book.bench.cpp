#include "trading-engine/order_book/order_book.hpp"

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
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
} // namespace

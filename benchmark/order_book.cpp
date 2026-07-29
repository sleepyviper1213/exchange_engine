#include "trading-engine/order_book/order_book.hpp"

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

using exchange::engine::order_book;
using exchange::price;
using exchange::quantity;
using exchange::side;
// Pre-generate a reproducible stream of prices so RNG cost is not timed.
namespace {
std::vector<price> makePrices(std::size_t n) {
	std::mt19937_64 rng(42);
	std::uniform_int_distribution<price> dist(1, 1'000'000);
	std::vector<price> prices;
	prices.reserve(n);
	for (std::size_t i = 0; i < n; ++i) prices.push_back(dist(rng));
	return prices;
}

// Insert N orders into a fresh book. Measures lower_bound + sorted insert.
void BM_AddOrder(benchmark::State &state) {
	const auto n      = static_cast<std::size_t>(state.range(0));
	const auto prices = makePrices(n);

	for (auto _ : state) {
		order_book book;
		for (auto p : prices) book.add_order(side::bid, p, 10);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() * n);
}

BENCHMARK(BM_AddOrder)
->RangeMultiplier(16)->Range(8, 8 << 10);

// Query best prices on a pre-filled book (pure read path).
void BM_GetBestPrices(benchmark::State &state) {
	const auto prices = makePrices(static_cast<std::size_t>(state.range(0)));
	order_book book;
	for (const auto p : prices) {
		book.add_order(side::bid, p, 10);
		book.add_order(side::ask, p, 10);
	}

	for (auto _ : state) {
		auto best = book.best_bid();
		benchmark::DoNotOptimize(best);
	}
}

BENCHMARK(BM_GetBestPrices)
->RangeMultiplier(8)->Range(8, 8 << 10);
} // namespace

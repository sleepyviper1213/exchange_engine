#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "engine/order_book.hpp"

// Pre-generate a reproducible stream of prices so RNG cost is not timed.
static std::vector<Price> makePrices(std::size_t n) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<Price> dist(1, 1'000'000);
    std::vector<Price> prices;
    prices.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        prices.push_back(dist(rng));
    }
    return prices;
}

static void BM_AddOrder_ExistingLevel(benchmark::State& state) {
    OrderBook book;

    constexpr Price price = 100000;
    book.add_order(Side::BID, price, 10);

    for (auto _ : state) {
        book.add_order(Side::BID, price, 10);
        benchmark::DoNotOptimize(book);
    }

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AddOrder_ExistingLevel);

static void BM_AddOrder_NewLevel(benchmark::State& state) {
    const std::size_t levels = static_cast<std::size_t>(state.range(0));

    std::vector<Price> prices;
    prices.reserve(levels);

    for (std::size_t i = 0; i < levels; ++i) {
        prices.push_back(static_cast<Price>(100000 + i));
    }

    for (auto _ : state) {
        OrderBook book;

        for (auto p : prices) {
            book.add_order(Side::ASK, p, 10);
        }

        benchmark::ClobberMemory();

        state.PauseTiming();
        const Price new_price = 100000 + static_cast<Price>(levels / 2);
        state.ResumeTiming();

        book.add_order(Side::ASK, new_price - 1, 10);

        benchmark::DoNotOptimize(book);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AddOrder_NewLevel)->RangeMultiplier(8)->Range(128, 8192);

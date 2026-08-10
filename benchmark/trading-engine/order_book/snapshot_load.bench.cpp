#include "market-data/binance/binance_depth.hpp"
#include "trading-engine/order_book/order_book.hpp"
#include "core/util/slurp.hpp"

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <cstdlib>
#include <fstream>
#include <string>
using namespace exchange::engine;
using namespace exchange::market_data;
using namespace exchange;

namespace {
using exchange::core::util::slurp;

// The depth snapshot under test, parsed (or synthesized) exactly once so the
// benchmark stays offline and deterministic — no network or JSON parsing in the
// timed region. Point OB_SNAPSHOT at a saved Binance depth JSON; otherwise this
// synthesizes 5000 bids + 5000 asks (~10k levels).
binance::DepthSnapshot snapshot() {
	if (const char *path = std::getenv("OB_SNAPSHOT")) {
		auto parsed = binance::parse_binance_depth(slurp(path), 2, 2);
		if (!parsed) std::abort();
		return *parsed;
	}
	binance::DepthSnapshot s;
	for (int i = 0; i < 5000; ++i) {
		s.bids.emplace_back(static_cast<price_t>(100'000 - i), 10);
		s.asks.emplace_back(static_cast<price_t>(100'001 + i), 10);
	}
	return s;
}

// Build a fresh OrderBook from the snapshot. Measures the sorted-insert path
// (lower_bound + level vector growth) at book-build scale.
void BM_LoadSnapshot(benchmark::State &state) {
	const auto snap   = snapshot();
	const auto levels = snap.bids.size() + snap.asks.size();

	for (auto _ : state) {
		order_book book;
		for (const auto &[price, qty] : snap.bids)
			book.add_order(side_t::bid, price, qty);
		for (const auto &[price, qty] : snap.asks)
			book.add_order(side_t::ask, price, qty);
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
	                        static_cast<std::int64_t>(levels));
	state.SetLabel(fmt::format("{} levels", levels));
}

BENCHMARK(BM_LoadSnapshot);
} // namespace
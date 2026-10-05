#include "core/util/slurp.hpp"
#include "market_data/binance/depth_snapshot.hpp"
#include "market_data/binance/parse_depth.hpp"
#include "order_book/order_book.hpp"

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <cstdlib>

using namespace exchange::engine;
using namespace exchange::market_data;
using namespace exchange;

namespace {
using exchange::core::util::slurp;

/// @brief A feed price as a tick count on this benchmark's 1:1 grid (scale 2,
///        tick 0.01), which is what the cast here always assumed.
[[nodiscard]] price_t snapshot_load_ticks(scaled_price_t price) noexcept {
	return at_tick(static_cast<std::uint32_t>(scaled_of(price)));
}

/// @brief A feed size as a lot count on the same 1:1 grid.
[[nodiscard]] quantity_t snapshot_load_lots(scaled_qty_t qty) noexcept {
	return static_cast<std::int32_t>(scaled_of(qty)) * units::lot;
}

// The depth snapshot under test, parsed (or synthesized) exactly once so the
// benchmark stays offline and deterministic - no network or JSON parsing in the
// timed region. Point OB_SNAPSHOT at a saved Binance depth JSON; otherwise this
// synthesizes 5000 bids + 5000 asks (~10k levels).
binance::depth_snapshot snapshot() {
	if (const char *path = std::getenv("OB_SNAPSHOT")) {
		auto parsed = binance::parse_binance_depth(slurp(path), 2, 2);
		if (!parsed) std::abort();
		return *parsed;
	}
	binance::depth_snapshot s;
	constexpr scaled_qty_t size = 10 * units::scaled_size;
	for (int i = 0; i < 5000; ++i) {
		s.bids.emplace_back(at_scaled(100'000 - i), size);
		s.asks.emplace_back(at_scaled(100'001 + i), size);
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
			book.add_order(side_t::bid,
						   snapshot_load_ticks(price),
						   snapshot_load_lots(qty));
		for (const auto &[price, qty] : snap.asks)
			book.add_order(side_t::ask,
						   snapshot_load_ticks(price),
						   snapshot_load_lots(qty));
		benchmark::DoNotOptimize(&book);
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(levels));
	state.SetLabel(fmt::format("{} levels", levels));
}

BENCHMARK(BM_LoadSnapshot);
} // namespace
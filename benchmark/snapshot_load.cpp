#include <benchmark/benchmark.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <fmt/format.h>

#include "../src/io/binance_depth.hpp"
#include "../src/engine/order_book.hpp"

namespace {
    // Read a whole file into a string.
    std::string slurp(const char *path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

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
            s.bids.emplace_back(
                static_cast<Price>(100'000 - i), 10
            );
            s.asks.emplace_back(
                static_cast<Price>(100'001 + i), 10
            );
        }
        return s;
    }

    // Build a fresh OrderBook from the snapshot. Measures the sorted-insert path
    // (lower_bound + level vector growth) at book-build scale.
    void BM_LoadSnapshot(benchmark::State &state) {
        const auto snap = snapshot();
        const auto levels = snap.bids.size() + snap.asks.size();

        for (auto _: state) {
            OrderBook book;
            for (const auto &[price, volume]: snap.bids)
                book.add_order(
                    Side::BID, price, volume);
            for (const auto &[price, volume]: snap.asks)
                book.add_order(
                    Side::ASK, price, volume);
            benchmark::DoNotOptimize(&book);
            benchmark::ClobberMemory();
        }
        state.SetItemsProcessed(
            state.iterations() * static_cast<std::int64_t>(levels));
        state.SetLabel(fmt::format("{} levels",levels));
    }

    BENCHMARK(BM_LoadSnapshot)->Unit(benchmark::kMicrosecond);
} // namespace

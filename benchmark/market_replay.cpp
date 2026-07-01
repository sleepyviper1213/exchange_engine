#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "io/binance_depth.hpp"
#include "engine/order_book.hpp"

// Real-world market replay: seed an OrderBook from a Binance REST depth snapshot,
// then stream a sequence of `depthUpdate` diff events through it — the managed
// local-order-book procedure Binance documents for the `<symbol>@depth` WebSocket
// feed. Each diff level is an *absolute* aggregated size (0 = remove), applied via
// OrderBook::set_level.
//
// The feed is offline and deterministic so the timed region has no network or JSON
// cost. Point OB_REPLAY at a JSONL capture of depthUpdate frames (one per line,
// e.g. from `websocat`); otherwise a representative stream is synthesized. Point
// OB_SNAPSHOT at a saved REST depth JSON for the seed book; otherwise a SOLUSDT-
// shaped book is synthesized. OB_PRICE_DECIMALS / OB_QTY_DECIMALS override the
// tick/step precision used to parse both files (default 2, matching SOLUSDT).
namespace {
    // SOLUSDT-shaped synthetic defaults: mid ~150.00, 0.01 tick, 2 decimals.
    constexpr int kDefaultDecimals = 2;
    constexpr Price kSynthMid = 15000; // 150.00 scaled by 10^2
    constexpr std::size_t kSynthDepth = 1000;
    // levels per side in the seed book
    constexpr std::size_t kSynthEvents = 5000;
    // diff events in the synthetic feed
    constexpr std::size_t kSynthTouchPerSide = 12;
    // levels touched per side per event
    constexpr std::size_t kSynthWindow = 200;
    // ticks around top-of-book a diff hits

    /**
     * @brief Read an integer environment variable, falling back on absence.
     * @param name Environment variable name.
     * @param fallback Value returned when @p name is unset.
     * @return The parsed integer, or @p fallback if the variable is not set.
     */
    int env_int(const char *name, int fallback) {
        if (const char *raw = std::getenv(name)) {
            return std::atoi(raw);
        }
        return fallback;
    }

    /**
     * @brief Read an entire file into a string.
     * @param path Filesystem path to read.
     * @return The file contents (empty if the file is missing or empty).
     */
    std::string slurp(const char *path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /**
     * @brief Obtain the seed order book snapshot.
     * @param price_decimals Tick precision used to parse an OB_SNAPSHOT file.
     * @param qty_decimals Step precision used to parse an OB_SNAPSHOT file.
     * @return The snapshot parsed from OB_SNAPSHOT, or a synthesized
     *         SOLUSDT-shaped book when the variable is unset.
     * @note Aborts if an OB_SNAPSHOT file is set but fails to parse.
     */
    binance::DepthSnapshot snapshot(int price_decimals, int qty_decimals) {
        if (const char *path = std::getenv("OB_SNAPSHOT")) {
            auto parsed =
                    binance::parse_binance_depth(slurp(path), price_decimals,
                                                 qty_decimals);
            if (!parsed) std::abort();
            return *parsed;
        }
        binance::DepthSnapshot s;
        s.bids.reserve(kSynthDepth);
        s.asks.reserve(kSynthDepth);
        for (std::size_t i = 0; i < kSynthDepth; ++i) {
            const auto tick = static_cast<Price>(i);
            s.bids.emplace_back(kSynthMid - tick, 100);
            s.asks.push_back({kSynthMid + 1 + tick, 100});
        }
        return s;
    }

    /**
     * @brief Synthesize a representative diff-depth stream from a seed book.
     *
     * Each event re-sizes a small window of near-touch levels, occasionally to 0
     * (a cancel/removal), mirroring how a live @c \@depth feed churns the top of
     * book. Deterministic: a fixed RNG seed keeps the timed region reproducible.
     * @param seed The book the stream perturbs (top of book anchors the window).
     * @return The synthesized diff events.
     */
    std::vector<binance::DepthUpdate>
    synth_updates(const binance::DepthSnapshot &seed) {
        const Price best_bid = seed.bids.empty()
                                   ? kSynthMid
                                   : seed.bids.front().price;
        const Price best_ask = seed.asks.empty()
                                   ? kSynthMid + 1
                                   : seed.asks.front().price;

        std::mt19937_64 rng(1234567);
        std::uniform_int_distribution<Price> off(0, kSynthWindow);
        std::uniform_int_distribution<Volume> qty(0, 200); // 0 ~ removal
        std::uniform_int_distribution<int> drift(-2, 2);

        std::vector<binance::DepthUpdate> updates;
        updates.reserve(kSynthEvents);
        Price bid_ref = best_bid;
        Price ask_ref = best_ask;
        std::uint64_t update_id = 1;
        for (std::size_t e = 0; e < kSynthEvents; ++e) {
            binance::DepthUpdate u;
            u.firstUpdateId = update_id;
            for (std::size_t k = 0; k < kSynthTouchPerSide; ++k) {
                u.bids.push_back({bid_ref - off(rng), qty(rng)});
                u.asks.push_back({ask_ref + off(rng), qty(rng)});
            }
            update_id += u.bids.size() + u.asks.size();
            u.finalUpdateId = update_id - 1;
            updates.push_back(std::move(u));

            // Wander the reference prices a little so the touched window moves.
            bid_ref = static_cast<Price>(
                static_cast<std::int64_t>(bid_ref) + drift(rng));
            ask_ref = static_cast<Price>(
                static_cast<std::int64_t>(ask_ref) + drift(rng));
        }
        return updates;
    }

    /**
     * @brief Obtain the diff-depth feed to replay.
     * @param seed Seed book used when synthesizing (ignored for OB_REPLAY files).
     * @param price_decimals Tick precision used to parse an OB_REPLAY file.
     * @param qty_decimals Step precision used to parse an OB_REPLAY file.
     * @return The events parsed from OB_REPLAY, or a synthesized stream when the
     *         variable is unset.
     * @note Aborts if an OB_REPLAY file is set but fails to parse.
     */
    std::vector<binance::DepthUpdate>
    updates(const binance::DepthSnapshot &seed, int price_decimals,
            int qty_decimals) {
        if (const char *path = std::getenv("OB_REPLAY")) {
            auto parsed =
                    binance::parse_binance_depth_updates(
                        slurp(path), price_decimals,
                        qty_decimals);
            if (!parsed) std::abort();
            return *parsed;
        }
        return synth_updates(seed);
    }

    /**
     * @brief Seed @p book with a snapshot's levels via absolute set_level updates.
     * @param book Book to populate (assumed empty).
     * @param snap Snapshot whose bid/ask levels are inserted.
     */
    void seed_book(OrderBook &book, const binance::DepthSnapshot &snap) {
        for (const auto &[price, volume]: snap.bids)
            book.set_level(Side::BID, price, volume);
        for (const auto &[price, volume]: snap.asks)
            book.set_level(Side::ASK, price, volume);
    }

    /**
     * @brief Apply one diff-depth event to @p book (absolute sizes, 0 = remove).
     * @param book Book to mutate.
     * @param u The diff event whose bid/ask levels are set.
     */
    void apply(OrderBook &book, const binance::DepthUpdate &u) {
        for (const auto &[price, volume]: u.bids)
            book.set_level(Side::BID, price, volume);
        for (const auto &[price, volume]: u.asks)
            book.set_level(Side::ASK, price, volume);
    }

    /**
     * @brief The offline replay input, materialized once outside the timed region.
     */
    struct ReplayData {
        binance::DepthSnapshot snap; ///< seed book
        std::vector<binance::DepthUpdate> feed; ///< diff events to replay
        std::size_t levels = 0; ///< total touched levels (items)
    };

    /**
     * @brief Load the seed book and diff feed and count the feed's total levels.
     * @return A ReplayData with snapshot, feed, and touched-level count populated.
     */
    ReplayData load() {
        const int pd = env_int("OB_PRICE_DECIMALS", kDefaultDecimals);
        const int qd = env_int("OB_QTY_DECIMALS", kDefaultDecimals);
        ReplayData data{
            snapshot(pd, qd),
            updates(data.snap, pd, qd)
        };
        for (const auto &u: data.feed)
            data.levels += u.bids.size() + u.asks.size();
        return data;
    }

    /**
     * @brief Steady-state replay throughput: apply the diff feed to a warm book.
     *
     * Because diffs set absolute sizes, re-running the same feed keeps the book
     * bounded, so this isolates pure update throughput (levels/sec) from book
     * construction.
     * @param state Google Benchmark state.
     */
    void BM_MarketReplay_SteadyState(benchmark::State &state) {
        const auto [snap, feed, levels] = load();

        OrderBook book;
        seed_book(book, snap);

        for (auto _: state) {
            for (const auto &u: feed) apply(book, u);
            benchmark::DoNotOptimize(&book);
            benchmark::ClobberMemory();
        }
        state.SetItemsProcessed(
            state.iterations() * static_cast<std::int64_t>(levels));
        state.SetLabel(fmt::format("{} events / {} levels", feed.size(),
                                   levels));
    }

    /**
     * @brief Cold end-to-end replay: rebuild the book from the snapshot and replay
     *        the whole feed each iteration (seed-plus-replay latency).
     * @param state Google Benchmark state.
     */
    void BM_MarketReplay_Cold(benchmark::State &state) {
        const auto [snap, feed, levels] = load();

        for (auto _: state) {
            OrderBook book;
            seed_book(book, snap);
            for (const auto &u: feed) apply(book, u);
            benchmark::DoNotOptimize(&book);
            benchmark::ClobberMemory();
        }
        state.SetItemsProcessed(
            state.iterations() * static_cast<std::int64_t>(levels));
    }

    BENCHMARK(BM_MarketReplay_SteadyState)->Unit(benchmark::kMicrosecond);
    BENCHMARK(BM_MarketReplay_Cold)->Unit(benchmark::kMicrosecond);
} // namespace

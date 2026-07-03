#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <fmt/format.h>

#include "io/binance_depth.hpp"
#include "engine/order_book.hpp"

// Tick-to-book latency: the steady-state hot path of a live order book. A REST
// snapshot seeds the book *once* at startup (not timed here — see snapshot_load);
// thereafter every `depthUpdate` frame off the `<symbol>@depth` WebSocket must be
// parsed and applied. THAT per-frame cost is what a low-latency deployment tunes,
// so both parse (`parse_binance_depth_update`) and apply (`set_level`) sit inside
// the timed region, and each frame is timed individually to report a latency
// distribution (p50/p99/tail) rather than a single throughput mean.
//
// Offline and deterministic: point OB_REPLAY at a JSONL capture of depthUpdate
// frames (one per line, e.g. from `websocat`); otherwise a representative stream is
// synthesized. OB_SNAPSHOT seeds the book from a saved REST depth JSON; otherwise a
// SOLUSDT-shaped book is synthesized. OB_PRICE_DECIMALS / OB_QTY_DECIMALS override
// the tick/step precision (default 2, matching SOLUSDT).
namespace {
    inline constexpr int kDefaultDecimals = 2;
    inline constexpr Price kSynthMid = 15000; // 150.00 scaled by 10^2
    inline constexpr std::size_t kSynthDepth = 1000; // seed levels per side
    inline constexpr std::size_t kSynthEvents = 5000; // frames in the feed
    inline constexpr std::size_t kSynthTouchPerSide = 12; // levels/side/frame
    inline constexpr Price kSynthWindow = 200; // ticks around top-of-book a frame hits
    inline constexpr Volume kSynthMaxQty = 200; // max synthesized size (0 = removal)

    using Clock = std::chrono::steady_clock;

    /// @brief 10^n as an integer, for un-scaling integral prices back to decimals.
    constexpr std::int64_t pow10i(int n) {
        std::int64_t r = 1;
        for (int i = 0; i < n; ++i) r *= 10;
        return r;
    }

    /**
     * @brief Read an integer environment variable, falling back on absence.
     * @param name Environment variable name.
     * @param fallback Value returned when @p name is unset.
     * @return The parsed integer, or @p fallback if the variable is not set.
     */
    int env_int(const char *name, int fallback) {
        if (const char *raw = std::getenv(name)) return std::atoi(raw);
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
     * @brief Format a scaled integer back into its decimal-string form.
     * @param value Integer scaled by 10^decimals.
     * @param decimals Number of fractional digits.
     * @return e.g. @c unscale(15000, 2) -> @c "150.00".
     */
    std::string unscale(std::int64_t value, int decimals) {
        if (decimals <= 0) return fmt::format("{}", value);
        const std::int64_t scale = pow10i(decimals);
        return fmt::format("{}.{:0{}d}", value / scale, value % scale, decimals);
    }

    /// @brief One (price, qty) pair as scaled integers, pre-un-scaling.
    using RawPair = std::pair<Price, Volume>;

    /**
     * @brief Render a bid/ask array as Binance @c [["price","qty"],...] JSON.
     * @param levels Scaled (price, qty) pairs.
     * @param price_decimals Tick precision to un-scale prices by.
     * @param qty_decimals Step precision to un-scale quantities by.
     * @return The JSON array text.
     */
    std::string levels_json(const std::vector<RawPair> &levels,
                            int price_decimals, int qty_decimals) {
        std::string out = "[";
        for (std::size_t i = 0; i < levels.size(); ++i) {
            if (i != 0) out += ',';
            out += fmt::format("[\"{}\",\"{}\"]",
                               unscale(levels[i].first, price_decimals),
                               unscale(levels[i].second, qty_decimals));
        }
        out += ']';
        return out;
    }

    /**
     * @brief Synthesize a representative feed of raw @c depthUpdate JSON frames.
     *
     * Each frame re-sizes a small window of near-touch levels, occasionally to 0 (a
     * cancel/removal), mirroring how a live @c \@depth feed churns the top of book.
     * Deterministic: a fixed RNG seed keeps the timed region reproducible. Frames
     * are emitted as real JSON text so the parser is exercised, not bypassed.
     * @param price_decimals Tick precision to un-scale prices by.
     * @param qty_decimals Step precision to un-scale quantities by.
     * @return The synthesized JSON frames, one string per @c depthUpdate.
     */
    std::vector<std::string> synth_frames(int price_decimals, int qty_decimals) {
        std::mt19937_64 rng(1234567);
        std::uniform_int_distribution<Price> off(0, kSynthWindow);
        std::uniform_int_distribution<Volume> qty(0, kSynthMaxQty); // 0 ~ removal
        std::uniform_int_distribution<int> drift(-2, 2);

        std::vector<std::string> frames;
        frames.reserve(kSynthEvents);
        Price bid_ref = kSynthMid;
        Price ask_ref = kSynthMid + 1;
        std::uint64_t update_id = 1;
        std::vector<RawPair> bids;
        std::vector<RawPair> asks;
        for (std::size_t e = 0; e < kSynthEvents; ++e) {
            bids.clear();
            asks.clear();
            for (std::size_t k = 0; k < kSynthTouchPerSide; ++k) {
                bids.emplace_back(bid_ref - off(rng), qty(rng));
                asks.emplace_back(ask_ref + off(rng), qty(rng));
            }
            const std::uint64_t first_id = update_id;
            update_id += bids.size() + asks.size();
            frames.push_back(fmt::format(
                R"({{"e":"depthUpdate","E":{},"s":"SOLUSDT","U":{},"u":{},"b":{},"a":{}}})",
                e, first_id, update_id - 1,
                levels_json(bids, price_decimals, qty_decimals),
                levels_json(asks, price_decimals, qty_decimals)));

            // Wander the reference prices so the touched window moves.
            bid_ref = static_cast<Price>(
                static_cast<std::int64_t>(bid_ref) + drift(rng));
            ask_ref = static_cast<Price>(
                static_cast<std::int64_t>(ask_ref) + drift(rng));
        }
        return frames;
    }

    /**
     * @brief Obtain the raw JSON @c depthUpdate frames to replay.
     * @param price_decimals Tick precision (also used to un-scale synthetic frames).
     * @param qty_decimals Step precision (also used to un-scale synthetic frames).
     * @return Frames from OB_REPLAY (one per non-blank line), or a synthesized feed.
     */
    std::vector<std::string> frames(int price_decimals, int qty_decimals) {
        if (const char *path = std::getenv("OB_REPLAY")) {
            const std::string text = slurp(path);
            std::vector<std::string> out;
            std::size_t pos = 0;
            while (pos <= text.size()) {
                const std::size_t nl = text.find('\n', pos);
                const std::size_t end = nl == std::string::npos
                                            ? text.size()
                                            : nl;
                std::string line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.find_first_not_of(" \t") != std::string::npos) {
                    out.push_back(std::move(line));
                }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            return out;
        }
        return synth_frames(price_decimals, qty_decimals);
    }

    /**
     * @brief Seed the book from a REST snapshot (OB_SNAPSHOT), or synthesize one.
     * @param book Book to populate (assumed empty).
     * @param price_decimals Tick precision used to parse an OB_SNAPSHOT file.
     * @param qty_decimals Step precision used to parse an OB_SNAPSHOT file.
     */
    void seed_book(OrderBook &book, int price_decimals, int qty_decimals) {
        binance::DepthSnapshot snap;
        if (const char *path = std::getenv("OB_SNAPSHOT")) {
            auto parsed = binance::parse_binance_depth(slurp(path), price_decimals,
                                                       qty_decimals);
            if (!parsed) std::abort();
            snap = std::move(*parsed);
        } else {
            snap.bids.reserve(kSynthDepth);
            snap.asks.reserve(kSynthDepth);
            for (std::size_t i = 0; i < kSynthDepth; ++i) {
                const auto tick = static_cast<Price>(i);
                snap.bids.emplace_back(kSynthMid - tick, 100);
                snap.asks.emplace_back(kSynthMid + 1 + tick, 100);
            }
        }
        for (const auto &[price, volume]: snap.bids)
            book.set_level(Side::BID, price, volume);
        for (const auto &[price, volume]: snap.asks)
            book.set_level(Side::ASK, price, volume);
    }

    /**
     * @brief The p-th percentile of @p samples via linear interpolation.
     * @param samples Latency samples (mutated: sorted in place).
     * @param p Percentile in [0, 100].
     * @return The interpolated percentile, or 0 if @p samples is empty.
     */
    double percentile(std::vector<double> &samples, double p) {
        if (samples.empty()) return 0.0;
        std::ranges::sort(samples);
        const double rank = p / 100.0 * static_cast<double>(samples.size() - 1);
        const auto lo = static_cast<std::size_t>(rank);
        if (lo + 1 >= samples.size()) return samples.back();
        const double frac = rank - static_cast<double>(lo);
        return samples[lo] + (samples[lo + 1] - samples[lo]) * frac;
    }

    /**
     * @brief Drive the per-frame tick-to-book loop with a caller-supplied parser.
     *
     * Because diffs set *absolute* sizes, re-running the same feed keeps the book
     * bounded, so repeated iterations stay in steady state. Each frame is timed
     * individually; percentiles are reported from the final (warm) pass. Note the
     * per-frame clock reads add a fixed overhead (~tens of ns on QPC) that inflates
     * absolute figures slightly — read these as a distribution shape, not an
     * absolute floor. Both parse strategies share this harness so their numbers are
     * directly comparable.
     * @param state Google Benchmark state.
     * @param feed The JSON @c depthUpdate frames to replay.
     * @param book The seeded book to mutate (kept warm across iterations).
     * @param parse_one Callable @c (std::string_view) -> expected<DepthUpdate,...>.
     */
    template <typename ParseOne>
    void run_ticks(benchmark::State &state, const std::vector<std::string> &feed,
                   OrderBook &book, ParseOne &&parse_one) {
        std::size_t parse_errors = 0;
        std::size_t levels = 0;
        double last_parse_ns = 0.0;
        double last_apply_ns = 0.0;
        std::vector<double> tick_ns; // per-frame latency of the final pass
        tick_ns.reserve(feed.size());

        for (auto _: state) {
            tick_ns.clear();
            double pass_parse_ns = 0.0;
            double pass_apply_ns = 0.0;
            double pass_total_ns = 0.0;
            std::size_t pass_levels = 0;
            for (const auto &frame: feed) {
                const auto t0 = Clock::now();
                auto update = parse_one(std::string_view(frame));
                const auto t1 = Clock::now();
                if (update) {
                    for (const auto &[price, volume]: update->bids)
                        book.set_level(Side::BID, price, volume);
                    for (const auto &[price, volume]: update->asks)
                        book.set_level(Side::ASK, price, volume);
                    pass_levels += update->bids.size() + update->asks.size();
                } else {
                    ++parse_errors;
                }
                const auto t2 = Clock::now();
                benchmark::DoNotOptimize(&book);

                using ns = std::chrono::duration<double, std::nano>;
                const double parse_ns = ns(t1 - t0).count();
                const double apply_ns = ns(t2 - t1).count();
                tick_ns.push_back(parse_ns + apply_ns);
                pass_parse_ns += parse_ns;
                pass_apply_ns += apply_ns;
                pass_total_ns += parse_ns + apply_ns;
            }
            benchmark::ClobberMemory();
            state.SetIterationTime(pass_total_ns / 1e9);
            last_parse_ns = pass_parse_ns;
            last_apply_ns = pass_apply_ns;
            levels = pass_levels;
        }

        state.SetItemsProcessed(
            state.iterations() * static_cast<std::int64_t>(levels));
        // Latency distribution (per frame) from the final warm pass.
        const auto n = static_cast<double>(tick_ns.empty() ? 1 : tick_ns.size());
        state.counters["p50_ns"] = percentile(tick_ns, 50.0);
        state.counters["p90_ns"] = percentile(tick_ns, 90.0);
        state.counters["p99_ns"] = percentile(tick_ns, 99.0);
        state.counters["p99.9_ns"] = percentile(tick_ns, 99.9);
        state.counters["max_ns"] = tick_ns.empty() ? 0.0 : tick_ns.back();
        // Mean parse-vs-apply split, so it's clear where each frame's time goes.
        state.counters["mean_parse_ns"] = last_parse_ns / n;
        state.counters["mean_apply_ns"] = last_apply_ns / n;
        state.SetLabel(fmt::format("{} frames / {} levels, {} parse errors",
                                   feed.size(), levels, parse_errors));
    }

    /**
     * @brief Cold path: a fresh simdjson parser + padded buffer per frame.
     * @param state Google Benchmark state.
     */
    void BM_TickToBook_Cold(benchmark::State &state) {
        const int pd = env_int("OB_PRICE_DECIMALS", kDefaultDecimals);
        const int qd = env_int("OB_QTY_DECIMALS", kDefaultDecimals);
        const auto feed = frames(pd, qd);
        OrderBook book;
        seed_book(book, pd, qd);
        run_ticks(state, feed, book, [pd, qd](std::string_view frame) {
            return binance::parse_binance_depth_update(frame, pd, qd);
        });
    }

    /**
     * @brief Warm path: one reusable DepthParser amortized across all frames.
     * @param state Google Benchmark state.
     */
    void BM_TickToBook_Warm(benchmark::State &state) {
        const int pd = env_int("OB_PRICE_DECIMALS", kDefaultDecimals);
        const int qd = env_int("OB_QTY_DECIMALS", kDefaultDecimals);
        const auto feed = frames(pd, qd);
        OrderBook book;
        seed_book(book, pd, qd);
        binance::DepthParser parser;
        run_ticks(state, feed, book, [&parser, pd, qd](std::string_view frame) {
            return parser.parse_update(frame, pd, qd);
        });
    }

    // UseManualTime: we report the summed per-frame work (excluding the loop's own
    // bookkeeping) via SetIterationTime, so throughput matches the timed region.
    BENCHMARK(BM_TickToBook_Cold)->UseManualTime()->Unit(benchmark::kMicrosecond);
    BENCHMARK(BM_TickToBook_Warm)->UseManualTime()->Unit(benchmark::kMicrosecond);
} // namespace

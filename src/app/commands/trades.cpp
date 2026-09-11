#include "trades.hpp"

#include "core/logging.hpp"
#include "core/metrics/quantile.hpp"
#include "core/scaled/decimal.hpp"
#include "core/util/slurp.hpp"
#include "market_data.hpp"
#include "market_data/format.hpp" // IWYU pragma: keep - feed_status

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exchange::app {
namespace {

namespace md = exchange::market_data;

/**
 * @brief A @ref md::trade_handler that measures the tape rather than trading on
 *        it.
 *
 * Everything it keeps is a summary: bucket counts, a price histogram and a few
 * running totals. It deliberately does not retain the prints - a busy symbol
 * records tens of thousands a minute, and holding them would make the tool's
 * memory a function of the recording's length for no gain.
 *
 * @note This is an offline reporting path, so it allocates freely. That is not
 *       a licence the ingest path has. @see the no-heap-traffic invariant in
 *       CLAUDE.md, which is about the matching and ingest paths and about
 *       nothing else.
 */
class tape_stats {
public:
	explicit tape_stats(std::int64_t bucket_ns) : bucket_ns_(bucket_ns) {}

	void on_trade(md::trade_print print) {
		const std::int64_t stamp = print.event_time.count();
		if (prints_ == 0) first_ns_ = stamp;
		last_ns_ = stamp;

		// The tape's only continuity signal. A venue trade id increments by one
		// per print on one symbol, so anything else is a print this process
		// never saw - and unlike a depth gap there is no snapshot to repair it
		// from, which is why it is counted rather than acted on.
		if (prints_ != 0 && print.id != previous_id_ + 1) ++discontinuities_;
		previous_id_ = print.id;

		++prints_;
		if (print.aggressor == side_t::bid) ++taker_bought_;
		else ++taker_sold_;

		++buckets_[stamp / bucket_ns_];
		++prices_[print.price];
		qty_ += print.qty;
	}

	/// @brief Bucket counts over the *occupied* span, empty buckets included.
	///
	/// Filling the gaps matters: a tape that is silent for a second and then
	/// prints four hundred times has a mean of 200/s only if the silent second
	/// is counted. Summarising over occupied buckets alone reports a market
	/// that is busier and far steadier than the one recorded.
	[[nodiscard]] std::vector<std::uint64_t> series() const {
		std::vector<std::uint64_t> counts;
		if (buckets_.empty()) return counts;
		const auto lo = buckets_.begin()->first;
		const auto hi = buckets_.rbegin()->first;
		counts.reserve(static_cast<std::size_t>(hi - lo) + 1);
		for (auto at = lo; at <= hi; ++at) {
			const auto found = buckets_.find(at);
			counts.push_back(found == buckets_.end() ? 0 : found->second);
		}
		return counts;
	}

	[[nodiscard]] std::uint64_t prints() const noexcept { return prints_; }

	[[nodiscard]] std::uint64_t discontinuities() const noexcept {
		return discontinuities_;
	}

	[[nodiscard]] std::uint64_t taker_bought() const noexcept {
		return taker_bought_;
	}

	[[nodiscard]] std::uint64_t taker_sold() const noexcept {
		return taker_sold_;
	}

	[[nodiscard]] std::int64_t qty() const noexcept { return qty_; }

	[[nodiscard]] double span_seconds() const noexcept {
		return prints_ < 2 ? 0.0
						   : double(last_ns_ - first_ns_) / 1'000'000'000.0;
	}

	[[nodiscard]] const std::unordered_map<std::int64_t, std::uint64_t> &
	prices() const noexcept {
		return prices_;
	}

private:
	std::int64_t bucket_ns_;
	std::uint64_t prints_          = 0;
	std::uint64_t discontinuities_ = 0;
	std::uint64_t taker_bought_    = 0;
	std::uint64_t taker_sold_      = 0;
	std::int64_t qty_              = 0;
	std::int64_t first_ns_         = 0;
	std::int64_t last_ns_          = 0;
	md::sequence_t previous_id_    = 0;
	/// Ordered, so series() can walk the occupied span without sorting keys.
	std::map<std::int64_t, std::uint64_t> buckets_;
	std::unordered_map<std::int64_t, std::uint64_t> prices_;
};

} // namespace

int cmd_trades(const std::string &file, int price_decimals, int qty_decimals,
			   int bucket_ms, unsigned long long max_trades) {
	using core::scaled::to_decimal;
	using exchange::core::util::slurp;

	if (bucket_ms <= 0) {
		spdlog::error("--bucket-ms must be positive (got {})", bucket_ms);
		return EXIT_FAILURE;
	}

	const std::string jsonl = slurp(file);
	if (jsonl.empty()) {
		spdlog::error("cannot read {} (missing or empty)", file);
		return EXIT_FAILURE;
	}

	md::binance::jsonl_trade_feed feed(jsonl, price_decimals, qty_decimals);
	tape_stats stats(std::int64_t(bucket_ms) * 1'000'000);
	// The whole point of the exercise: the same drive() a live tape would use,
	// over the same feed concept, with a handler that happens to count instead
	// of trade.
	// Qualified rather than found by ADL: the feed's namespace is
	// market_data::binance and drive() lives one level up in market_data, which
	// argument-dependent lookup does not search.
	const md::trade_run run = md::drive(feed, stats, max_trades);

	const bool clean =
		is_clean(run) && feed.malformed() == 0 && stats.discontinuities() == 0;
	fmt::println("tape {}  [{}]",
				 file,
				 clean ? "clean" : "SUSPECT - see the counters below");
	fmt::println("  feed      {} prints, {} malformed, {} id discontinuities"
				 " ({})",
				 run.trades,
				 feed.malformed(),
				 stats.discontinuities(),
				 run.stop.detail.empty() ? describe(run.stop.reason)
										 : run.stop.detail);

	const double span = stats.span_seconds();
	fmt::println("  market    {:.1f}s of market time, {} traded",
				 span,
				 core::scaled::to_decimal(stats.qty(), qty_decimals));

	std::vector<std::uint64_t> counts = stats.series();
	std::ranges::sort(counts);
	const double per_bucket =
		counts.empty() ? 0.0 : double(run.trades) / double(counts.size());
	const double to_rate = 1000.0 / double(bucket_ms);
	fmt::println("  rate      {:.1f}/s mean, {:.1f}/s peak"
				 " (p50 {}, p95 {}, p99 {} per {}ms)",
				 per_bucket * to_rate,
				 double(counts.empty() ? 0 : counts.back()) * to_rate,
				 core::metrics::quantile_of<std::uint64_t>(counts,
														   core::metrics::percentile::P50),
				 core::metrics::quantile_of<std::uint64_t>(counts,
														   core::metrics::percentile::P95),
				 core::metrics::quantile_of<std::uint64_t>(counts,
														   core::metrics::percentile::P99),
				 bucket_ms);

	// Variance over mean: 1 for a Poisson arrival process, and far above it for
	// a real tape. It is the one number that says "bursty" rather than merely
	// "fast", which is the property a recording is kept for.
	double variance = 0.0;
	for (const auto count : counts) {
		const double delta = double(count) - per_bucket;
		variance += delta * delta;
	}
	if (!counts.empty()) variance /= double(counts.size());
	const auto idle =
		static_cast<std::size_t>(std::ranges::count(counts, std::uint64_t{0}));
	fmt::println(
		"  burst     var/mean {:.1f} over {} buckets, {} idle ({:.1f}%)",
		per_bucket > 0.0 ? variance / per_bucket : 0.0,
		counts.size(),
		idle,
		counts.empty() ? 0.0 : 100.0 * double(idle) / double(counts.size()));

	const auto total = std::max<std::uint64_t>(1, run.trades);
	fmt::println(
		"  flow      {} taker bought ({:.1f}%), {} taker sold ({:.1f}%)",
		stats.taker_bought(),
		100.0 * double(stats.taker_bought()) / double(total),
		stats.taker_sold(),
		100.0 * double(stats.taker_sold()) / double(total));

	std::int64_t busiest       = 0;
	std::uint64_t busiest_hits = 0;
	for (const auto &[price, hits] : stats.prices())
		if (hits > busiest_hits) {
			busiest      = price;
			busiest_hits = hits;
		}
	fmt::println("  prices    {} distinct over {} prints ({:.1f} per price),"
				 " busiest {} x{}",
				 stats.prices().size(),
				 run.trades,
				 stats.prices().empty()
					 ? 0.0
					 : double(run.trades) / double(stats.prices().size()),
				 core::scaled::to_decimal(busiest, price_decimals),
				 busiest_hits);

	// A discontinuity is a fact about the recording, not a failure of this
	// command, so it is loud in the label and absent from the exit code.
	// Failing to reach the end of the file is the other way round. @see
	// cmd_replay and cmd_backtest, which draw the line in the same place.
	if (!is_clean(run)) {
		spdlog::error("the tape did not replay to the end: {}", run.stop);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

} // namespace exchange::app

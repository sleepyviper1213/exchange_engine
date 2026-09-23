// Where new price levels are born and die in the ladder, and what that costs
// each candidate layout.
//
// `book_side` keeps its levels in an intrusive red-black tree. The standing
// question is whether a flat price-sorted array would beat it, and the answer
// turns on one number this file measures: the distribution of *insert rank* -
// how far from the touch a newly created level lands. A tree pays
// ~log2(n) dependent loads regardless of rank; a flat array pays the same
// binary search plus a shift proportional to rank. So the array wins only if
// levels are born near the touch, and the shift stays short.
//
// Two halves:
//
//   BM_LadderPos_Distribution reports the rank histogram and the shift each
//   layout would pay, in elements moved. That is arithmetic over the trace, not
//   a timing - it says what the workload *is*.
//
//   BM_LadderPos_{Tree,BestFirst,WorstFirst} replay the same trace against the
//   actual structures. That is the timing.
//
// @warning The default feed is synthetic and its answer is baked in:
//          replay.fixture.hpp's SYNTH_WINDOW confines every diff to 200 ticks
//          around the touch of a 1000-level book, so near-touch births are a
//          property of the generator rather than of markets. Only an
//          OB_SNAPSHOT/OB_REPLAY run decides anything; the label says which you
//          got.

#include "market_data/replay.fixture.hpp"
#include "orders/types.hpp"

#include <benchmark/benchmark.h>
#include <boost/intrusive/set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

using namespace exchange;
using namespace exchange::market_data;

namespace {

using exchange::price_t;
using exchange::side_t;
using exchange::volume_t;

/// @brief Does @p a rest closer to the touch than @p b? Bids sort descending,
///        asks ascending - the ladder order `book_side` itself uses.
[[nodiscard]] constexpr bool ladder_pos_before(side_t side, price_t a,
											   price_t b) noexcept {
	return side == side_t::bid ? a > b : a < b;
}

// --- the ladder as a flat array, used to derive ranks ------------------------

/**
 * @brief One side's live prices, best at [0].
 *
 * The measuring instrument and one of the candidates at once: deriving a rank
 * *is* maintaining a sorted array, so the trace-building pass exercises the
 * layout it is collecting evidence about.
 */
class ladder_pos_mirror {
public:
	static constexpr std::size_t ABSENT = static_cast<std::size_t>(-1);

	explicit ladder_pos_mirror(side_t side) noexcept : side_(side) {}

	[[nodiscard]] std::size_t size() const noexcept { return prices_.size(); }

	[[nodiscard]] const std::vector<price_t> &prices() const noexcept {
		return prices_;
	}

	/// @brief Distance from the touch, or @c ABSENT if no level rests there.
	[[nodiscard]] std::size_t rank_of(price_t price) const {
		const auto at = seek(price);
		return at != prices_.end() && *at == price
				   ? static_cast<std::size_t>(at - prices_.begin())
				   : ABSENT;
	}

	/// @brief Create the level. @return The rank it landed at.
	std::size_t insert(price_t price) {
		const auto at   = seek(price);
		const auto rank = static_cast<std::size_t>(at - prices_.begin());
		prices_.insert(at, price);
		return rank;
	}

	/// @brief Drop the level. @pre It rests. @return The rank it was at.
	void erase_at(std::size_t rank) {
		prices_.erase(prices_.begin() + static_cast<std::ptrdiff_t>(rank));
	}

private:
	[[nodiscard]] std::vector<price_t>::const_iterator
	seek(price_t price) const {
		return std::lower_bound(prices_.begin(), prices_.end(), price,
								[this](price_t a, price_t b) {
									return ladder_pos_before(side_, a, b);
								});
	}

	std::vector<price_t> prices_;
	side_t side_;
};

// --- the trace ---------------------------------------------------------------

/// @brief One level birth or death, and where in the ladder it happened.
struct ladder_pos_event {
	price_t price;
	std::uint32_t rank;  ///< distance from the touch; 0 = the level is the touch
	std::uint32_t depth; ///< levels resting on that side *before* the op
	side_t side;
	bool is_insert;
};

struct ladder_pos_trace {
	std::vector<ladder_pos_event> events;
	std::vector<price_t> seed_bids; ///< state the trace both starts and ends in
	std::vector<price_t> seed_asks;
	std::size_t peak_bids  = 0; ///< pool sizing for the tree candidate
	std::size_t peak_asks  = 0;
	std::size_t feed_levels = 0;
	bool is_real_data       = false;
};

/// @brief Apply one absolute L2 size, recording the birth or death it causes.
///
/// A diff carries a level's whole new size, so a level is born when a price
/// that rested nothing becomes non-zero and dies when it returns to zero.
/// Everything between is a resize, which moves no level and is not an event.
void ladder_pos_apply(ladder_pos_mirror &mirror, side_t side, price_t price,
					  volume_t qty, std::vector<ladder_pos_event> *out) {
	const std::size_t rank  = mirror.rank_of(price);
	const bool rests        = rank != ladder_pos_mirror::ABSENT;
	const auto depth        = static_cast<std::uint32_t>(mirror.size());

	if (qty > 0 && !rests) {
		const auto at = static_cast<std::uint32_t>(mirror.insert(price));
		if (out != nullptr)
			out->push_back({price, at, depth, side, true});
	} else if (qty <= 0 && rests) {
		mirror.erase_at(rank);
		if (out != nullptr)
			out->push_back(
				{price, static_cast<std::uint32_t>(rank), depth, side, false});
	}
}

/**
 * @brief Build the trace once: seed, settle, then record a second pass.
 *
 * The first pass over the feed is discarded on purpose. Diffs carry absolute
 * sizes, so a level's value after a pass is set by the last event that touched
 * it - which makes a second pass from that state end exactly where it began.
 * Recording the settled pass buys two things: the births and deaths net out, so
 * a candidate structure can replay the trace any number of times without
 * drifting, and the seed snapshot's own construction (which appends every level
 * at the back, rank == depth every time) stays out of the histogram.
 */
[[nodiscard]] const ladder_pos_trace &ladder_pos_load() {
	static const ladder_pos_trace trace = [] {
		const auto data = replay::load();

		ladder_pos_mirror bids(side_t::bid);
		ladder_pos_mirror asks(side_t::ask);
		ladder_pos_trace built;

		const auto apply_update = [&](const binance::depth_update &u,
									  std::vector<ladder_pos_event> *out) {
			for (const auto &[price, qty] : u.bids)
				ladder_pos_apply(bids, side_t::bid, static_cast<price_t>(price),
								 static_cast<volume_t>(qty), out);
			for (const auto &[price, qty] : u.asks)
				ladder_pos_apply(asks, side_t::ask, static_cast<price_t>(price),
								 static_cast<volume_t>(qty), out);
			built.peak_bids = std::max(built.peak_bids, bids.size());
			built.peak_asks = std::max(built.peak_asks, asks.size());
		};

		for (const auto &[price, qty] : data.snap.bids)
			ladder_pos_apply(bids, side_t::bid, static_cast<price_t>(price),
							 static_cast<volume_t>(qty), nullptr);
		for (const auto &[price, qty] : data.snap.asks)
			ladder_pos_apply(asks, side_t::ask, static_cast<price_t>(price),
							 static_cast<volume_t>(qty), nullptr);

		for (const auto &u : data.feed) apply_update(u, nullptr); // settle

		built.seed_bids = bids.prices();
		built.seed_asks = asks.prices();
		built.peak_bids = std::max(built.peak_bids, bids.size());
		built.peak_asks = std::max(built.peak_asks, asks.size());

		for (const auto &u : data.feed) apply_update(u, &built.events);

		built.feed_levels  = data.levels;
		built.is_real_data = std::getenv("OB_REPLAY") != nullptr ||
							 std::getenv("OB_SNAPSHOT") != nullptr;
		return built;
	}();
	return trace;
}

[[nodiscard]] std::string ladder_pos_label(const ladder_pos_trace &t) {
	return fmt::format("{} level ops / depth {}+{} / {}",
					   t.events.size(),
					   t.seed_bids.size(),
					   t.seed_asks.size(),
					   t.is_real_data ? "REAL feed"
									  : "SYNTHETIC feed (window-bounded)");
}

// --- what each layout would shift --------------------------------------------

/**
 * @brief Elements a flat ladder moves for one event, under three layouts.
 *
 * @c best_first keeps the touch at [0], so every op at the touch - the common
 * one - shifts the whole ladder. @c worst_first keeps it at @c back(), making
 * erase-at-touch a @c pop_back and a new best a @c push_back, and paying for
 * ops at the far end instead. @c ring adds a head index and shifts whichever
 * side is shorter, which is the lower bound on any shifting layout.
 */
struct ladder_pos_shift {
	std::uint32_t best_first;
	std::uint32_t worst_first;
	std::uint32_t ring;
};

[[nodiscard]] constexpr ladder_pos_shift
ladder_pos_shift_for(const ladder_pos_event &e) noexcept {
	// Levels lying between the op and each end of the ladder. An insert splits
	// `depth` in two; an erase removes itself from the count first.
	const std::uint32_t toward_touch = e.rank;
	const std::uint32_t toward_far =
		e.is_insert ? e.depth - e.rank : e.depth - e.rank - 1;
	return {.best_first  = toward_far,
			.worst_first = toward_touch,
			.ring        = std::min(toward_touch, toward_far)};
}

constexpr std::size_t LADDER_POS_BUCKETS = 12;
constexpr std::array<const char *, LADDER_POS_BUCKETS> LADDER_POS_LABELS = {
	"0",      "1",       "2-3",     "4-7",      "8-15",     "16-31",
	"32-63",  "64-127",  "128-255", "256-511",  "512-1023", "1024+"};

[[nodiscard]] std::size_t ladder_pos_bucket(std::uint32_t v) noexcept {
	return v == 0 ? 0
				  : std::min<std::size_t>(LADDER_POS_BUCKETS - 1,
										  static_cast<std::size_t>(
											  std::bit_width(v)));
}

[[nodiscard]] std::uint32_t
ladder_pos_percentile(std::vector<std::uint32_t> &v, double q) {
	if (v.empty()) return 0;
	const auto at = static_cast<std::size_t>(
		q * static_cast<double>(v.size() - 1));
	std::nth_element(v.begin(),
					 v.begin() + static_cast<std::ptrdiff_t>(at),
					 v.end());
	return v[at];
}

/**
 * @brief Report the rank histogram and per-layout shift cost.
 *
 * Registered at one iteration: it computes over a fixed trace, so repeating it
 * would report the cost of the arithmetic rather than anything about the book.
 */
void BM_LadderPos_Distribution(benchmark::State &state) {
	const auto &trace = ladder_pos_load();

	std::array<std::size_t, LADDER_POS_BUCKETS> insert_hist{};
	std::array<std::size_t, LADDER_POS_BUCKETS> erase_hist{};
	std::vector<std::uint32_t> best_first;
	std::vector<std::uint32_t> worst_first;
	std::vector<std::uint32_t> ring;
	std::size_t inserts = 0;
	std::size_t at_touch = 0;
	std::size_t within_32 = 0;
	std::size_t depth_sum = 0;

	best_first.reserve(trace.events.size());
	worst_first.reserve(trace.events.size());
	ring.reserve(trace.events.size());

	for (const auto &e : trace.events) {
		(e.is_insert ? insert_hist : erase_hist)[ladder_pos_bucket(e.rank)]++;
		inserts += static_cast<std::size_t>(e.is_insert);
		at_touch += static_cast<std::size_t>(e.rank == 0);
		within_32 += static_cast<std::size_t>(e.rank < 32);
		depth_sum += e.depth;

		const auto shift = ladder_pos_shift_for(e);
		best_first.push_back(shift.best_first);
		worst_first.push_back(shift.worst_first);
		ring.push_back(shift.ring);
	}

	for (auto _ : state) benchmark::DoNotOptimize(depth_sum);

	const auto n = static_cast<double>(std::max<std::size_t>(
		trace.events.size(), 1));
	const auto mean = [n](const std::vector<std::uint32_t> &v) {
		return static_cast<double>(
				   std::accumulate(v.begin(), v.end(), std::uint64_t{0})) /
			   n;
	};

	fmt::print(stderr,
			   "\n--- ladder rank distribution ({}) ---\n"
			   "{:>10}  {:>10}  {:>10}\n",
			   trace.is_real_data ? "real feed" : "SYNTHETIC - see file header",
			   "rank",
			   "births",
			   "deaths");
	for (std::size_t i = 0; i < LADDER_POS_BUCKETS; ++i)
		if (insert_hist[i] != 0 || erase_hist[i] != 0)
			fmt::print(stderr, "{:>10}  {:>10}  {:>10}\n", LADDER_POS_LABELS[i],
					   insert_hist[i], erase_hist[i]);
	fmt::print(stderr,
			   "\nelements shifted per op   mean      p50      p90      p99\n"
			   "  best-first vector  {:>9.1f} {:>8} {:>8} {:>8}\n"
			   "  worst-first vector {:>9.1f} {:>8} {:>8} {:>8}\n"
			   "  ring (shift short) {:>9.1f} {:>8} {:>8} {:>8}\n"
			   "\na 16-byte cell moves ~1/cycle without AVX2; the tree pays"
			   " ~5*log2(depth) + rebalance.\n\n",
			   mean(best_first), ladder_pos_percentile(best_first, 0.50),
			   ladder_pos_percentile(best_first, 0.90),
			   ladder_pos_percentile(best_first, 0.99),
			   mean(worst_first), ladder_pos_percentile(worst_first, 0.50),
			   ladder_pos_percentile(worst_first, 0.90),
			   ladder_pos_percentile(worst_first, 0.99),
			   mean(ring), ladder_pos_percentile(ring, 0.50),
			   ladder_pos_percentile(ring, 0.90),
			   ladder_pos_percentile(ring, 0.99));

	state.counters["ops"]          = static_cast<double>(trace.events.size());
	state.counters["births"]       = static_cast<double>(inserts);
	state.counters["at_touch_pct"] = 100.0 * static_cast<double>(at_touch) / n;
	state.counters["top32_pct"]    = 100.0 * static_cast<double>(within_32) / n;
	state.counters["mean_depth"]   = static_cast<double>(depth_sum) / n;
	state.counters["shift_best_first"]  = mean(best_first);
	state.counters["shift_worst_first"] = mean(worst_first);
	state.counters["shift_ring"]        = mean(ring);
	state.SetLabel(ladder_pos_label(trace));
}

BENCHMARK(BM_LadderPos_Distribution)->Iterations(1);

// --- the candidates ----------------------------------------------------------

/// @brief What a flat ladder actually moves: price beside its aggregate.
///
/// The level's orders stay in the pool and are reached through a parallel
/// pointer array, so a shift never touches them - the objection `book_side`
/// records against a sorted vector applies to a vector *of levels*, not to this.
struct ladder_pos_cell {
	price_t price;
	volume_t volume;
};
static_assert(sizeof(ladder_pos_cell) == 16, "four cells to a cache line");

struct ladder_pos_node
	: boost::intrusive::set_base_hook<
		  boost::intrusive::link_mode<boost::intrusive::normal_link> > {
	price_t price;
	volume_t volume;
};

struct ladder_pos_node_order {
	side_t side;
	[[nodiscard]] bool operator()(const ladder_pos_node &a,
								  const ladder_pos_node &b) const noexcept {
		return ladder_pos_before(side, a.price, b.price);
	}
};

using ladder_pos_tree =
	boost::intrusive::set<ladder_pos_node,
						  boost::intrusive::compare<ladder_pos_node_order>,
						  boost::intrusive::constant_time_size<false> >;

/**
 * @brief The tree candidate: an intrusive set over pooled nodes.
 *
 * Erase takes the node rather than searching for it, because the real
 * `book_side` reaches its level through `by_price_` and unlinks with
 * `s_iterator_to`. The flat candidates below have no such shortcut - an index
 * cached anywhere is invalidated by the next shift - so they pay a search on
 * erase that this does not. That asymmetry is part of what is being measured.
 */
class ladder_pos_tree_side {
public:
	ladder_pos_tree_side(side_t side, const std::vector<price_t> &seed,
						 std::size_t capacity)
		: tree_(ladder_pos_node_order{side}) {
		storage_.resize(capacity);
		free_.reserve(capacity);
		for (std::size_t i = capacity; i-- > 0;) free_.push_back(i);
		by_price_.reserve(capacity * 2);
		for (const price_t price : seed) insert(price);
	}

	void insert(price_t price) {
		const std::size_t slot = free_.back();
		free_.pop_back();
		ladder_pos_node &node = storage_[slot];
		node.price            = price;
		node.volume           = 1;
		tree_.insert(node);
		by_price_.emplace(price, slot);
	}

	void erase(price_t price) {
		const auto found = by_price_.find(price);
		if (found == by_price_.end()) return;
		const std::size_t slot = found->second;
		tree_.erase(ladder_pos_tree::s_iterator_to(storage_[slot]));
		by_price_.erase(found);
		free_.push_back(slot);
	}

	[[nodiscard]] price_t best() const noexcept { return tree_.begin()->price; }

private:
	std::vector<ladder_pos_node> storage_;
	std::vector<std::size_t> free_;
	boost::unordered_flat_map<price_t, std::size_t> by_price_;
	ladder_pos_tree tree_;
};

/// @brief The flat candidate. @tparam BestAtFront which end holds the touch.
template <bool BestAtFront> class ladder_pos_flat_side {
public:
	ladder_pos_flat_side(side_t side, const std::vector<price_t> &seed,
						 std::size_t capacity)
		: side_(side) {
		cells_.reserve(capacity + 1);
		for (const price_t price : seed) insert(price);
	}

	void insert(price_t price) {
		cells_.insert(seek(price), ladder_pos_cell{.price = price, .volume = 1});
	}

	void erase(price_t price) {
		const auto at = seek(price);
		if (at != cells_.end() && at->price == price) cells_.erase(at);
	}

	[[nodiscard]] price_t best() const noexcept {
		return BestAtFront ? cells_.front().price : cells_.back().price;
	}

private:
	/// @brief Where @p price belongs, under whichever end holds the touch.
	[[nodiscard]] std::vector<ladder_pos_cell>::iterator seek(price_t price) {
		return std::lower_bound(cells_.begin(), cells_.end(), price,
								[this](const ladder_pos_cell &c, price_t p) {
									return BestAtFront
											   ? ladder_pos_before(side_,
																   c.price, p)
											   : ladder_pos_before(side_, p,
																   c.price);
								});
	}

	std::vector<ladder_pos_cell> cells_;
	side_t side_;
};

/// @brief Replay the trace against one candidate; the trace is balanced, so the
///        structure ends each pass exactly as it began.
template <typename Side>
void ladder_pos_replay(benchmark::State &state) {
	const auto &trace = ladder_pos_load();

	Side bids(side_t::bid, trace.seed_bids, trace.peak_bids + 1);
	Side asks(side_t::ask, trace.seed_asks, trace.peak_asks + 1);

	for (auto _ : state) {
		for (const auto &e : trace.events) {
			Side &side = e.side == side_t::bid ? bids : asks;
			if (e.is_insert) side.insert(e.price);
			else side.erase(e.price);
		}
		benchmark::DoNotOptimize(bids.best());
		benchmark::DoNotOptimize(asks.best());
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations() *
							static_cast<std::int64_t>(trace.events.size()));
	state.SetLabel(ladder_pos_label(trace));
}

void BM_LadderPos_Tree(benchmark::State &state) {
	ladder_pos_replay<ladder_pos_tree_side>(state);
}

void BM_LadderPos_BestFirst(benchmark::State &state) {
	ladder_pos_replay<ladder_pos_flat_side<true> >(state);
}

void BM_LadderPos_WorstFirst(benchmark::State &state) {
	ladder_pos_replay<ladder_pos_flat_side<false> >(state);
}

BENCHMARK(BM_LadderPos_Tree);
BENCHMARK(BM_LadderPos_BestFirst);
BENCHMARK(BM_LadderPos_WorstFirst);

} // namespace

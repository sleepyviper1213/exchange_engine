#include "normalise.hpp"

#include "binance_depth.hpp"
#include "market-data/fwd.hpp"
#include "market-data/normalised.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

namespace exchange::market_data::binance {
namespace {

/// Binance stamps events in milliseconds since the Unix epoch; the neutral feed
/// speaks nanoseconds, so the unit conversion happens once, here.
timestamp to_timestamp(std::uint64_t event_time_ms) noexcept {
	return std::chrono::duration_cast<timestamp>(
		std::chrono::milliseconds(static_cast<std::int64_t>(event_time_ms)));
}

/**
 * Copy a decoded side into neutral levels.
 *
 * @c PriceLevel is now an alias for @c book_level, so this *could* be
 * `return levels;` - one vector copy-construct instead of a per-element loop.
 * It was, briefly, and it measured slower: on BM_Reconstructor_SteadyState (the
 * stable metric, cv 3-5%) the loop runs 4.07 ms against the copy's 4.67 ms, and
 * the result held across an A-B-A rebuild.
 *
 * The likely reason is size. This corpus averages ~12 levels per side per event
 * - 192 bytes - and @c vector's copy constructor lowers to a @c memmove call
 * that cannot see the length at compile time, while the reserve-plus-emplace
 * loop inlines and vectorises for a known-trivial 16-byte element. Below some
 * threshold the call overhead dominates the copy, and a depth diff is well
 * below it.
 *
 * So the loop stays, and it stays *because it was measured*, not because the
 * types still differ - they do not. A venue whose frames carry hundreds of
 * levels per side would want the other form; re-run the benchmark before
 * switching.
 */
std::vector<book_level> to_levels(const std::vector<PriceLevel> &levels) {
	std::vector<book_level> normalised;
	normalised.reserve(levels.size());
	for (const auto &[price, volume] : levels)
		normalised.emplace_back(price, volume);
	return normalised;
}

/// Binance states U/u as @c uint64 on the wire; @c sequence_t is signed (see
/// market-data/fwd.hpp for why). The cast is explicit and checked rather than
/// implicit: an id past 2^63 would alias onto a negative sequence and make
/// every later comparison in @c depth_sequencer nonsense. Binance's update ids
/// are ~10^9, so the assertion states a property of the venue rather than
/// guarding a live concern.
constexpr sequence_t to_sequence(std::uint64_t wire_id) noexcept {
	assert(wire_id <= static_cast<std::uint64_t>(
						  std::numeric_limits<sequence_t>::max()) &&
		   "venue update id exceeds the signed sequence domain");
	return static_cast<sequence_t>(wire_id);
}

} // namespace

core::util::inclusive_range<sequence_t>
sequence_of(const DepthUpdateMeta &meta) noexcept {
	return core::util::inclusive_range<sequence_t>{
		to_sequence(meta.firstUpdateId),
		to_sequence(meta.finalUpdateId)};
}

core::util::inclusive_range<sequence_t>
sequence_of(const DepthUpdate &update) noexcept {
	return core::util::inclusive_range<sequence_t>{
		to_sequence(update.firstUpdateId),
		to_sequence(update.finalUpdateId)};
}

depth_event normalise(const DepthUpdate &update) {
	return depth_event{sequence_of(update),
					   to_timestamp(update.eventTime),
					   to_levels(update.bids),
					   to_levels(update.asks)};
}

book_snapshot normalise(const DepthSnapshot &snapshot) {
	// Same wire-to-sequence narrowing as the diff path, through the same
	// checked helper - a snapshot's lastUpdateId is what seeds the sequencer,
	// so an id that aliased here would set the expected sequence to a negative
	// number and make every diff that followed read as a gap.
	return book_snapshot{to_sequence(snapshot.lastUpdateId),
						 timestamp{},
						 to_levels(snapshot.bids),
						 to_levels(snapshot.asks)};
}

} // namespace exchange::market_data::binance

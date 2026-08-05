#pragma once
// Venue-neutral market data: the shape every feed decoder normalises into.
//
// A venue's own types speak its wire vocabulary. Binance names the update-id
// bounds of a diff `U` and `u`, stamps events in milliseconds, delivers bids
// descending and asks ascending, and spells sizes as decimal strings. Another
// venue names, orders and scales all four differently. None of that should
// reach the code that sequences a feed or reconstructs a book, so it stops at
// the decoder: each venue adapter supplies a `normalise()` overload (see
// binance/normalise.hpp) producing the types below, and everything downstream
// knows only these.
//
// What normalisation fixes:
//   * sequencing — one inclusive @ref sequence_range per event, whatever the
//     venue calls its bounds;
//   * time — nanoseconds since the Unix epoch, whatever resolution it publishes;
//   * levels — already scaled to the book's integral Price/Volume, in whatever
//     order they arrived (@c l2_book::load imposes the ordering).

#include "trading-engine/orders/types.hpp" // IWYU pragma: keep — Price/Volume via book_level
#include "fwd.hpp"
#include "l2_book.hpp"
#include "market_data_export.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace exchange::market_data {

/// @brief A normalised aggregated level. Identical in shape to the book's own
///        cell, so a decoded side moves into an @c l2_book with no conversion.
using book_level = l2_book::Level;

/// @brief Nanoseconds since the Unix epoch — the one time unit a normalised
///        feed speaks, whatever resolution the venue publishes.
using timestamp = std::chrono::nanoseconds;

/**
 * @brief The inclusive span of venue sequence numbers one event carries.
 *
 * Venues publish a diff as a range rather than a single number because one
 * frame may coalesce several book mutations: Binance's @c U / @c u are exactly
 * this. Sequencing an event is then a statement about the range — is the number
 * we are waiting for inside it — which is what @c depth_sequencer asks.
 *
 * A snapshot is the degenerate case: @c first == @c last == its update id.
 */
struct sequence_range {
	std::uint64_t first = 0; ///< First sequence number covered (inclusive).
	std::uint64_t last  = 0; ///< Last sequence number covered (inclusive).

	/// @brief Whether @p sequence falls inside this range.
	[[nodiscard]] constexpr bool covers(std::uint64_t sequence) const noexcept {
		return first <= sequence && sequence <= last;
	}

	/// @brief Whether the range is well-formed (@c first <= @c last). A frame
	///        that fails this cannot be sequenced and is treated as a gap.
	[[nodiscard]] constexpr bool ordered() const noexcept {
		return first <= last;
	}

	[[nodiscard]] friend constexpr bool
	operator==(const sequence_range &, const sequence_range &) = default;
};

/**
 * @brief One normalised depth diff: the levels a venue changed, plus the
 *        sequence range and time that place it in the feed.
 *
 * Each level is an @em absolute aggregate size, not a delta — a size of 0
 * removes the price. That is the L2 diff primitive @c l2_book::set_level takes,
 * and it is what makes replay idempotent for any event that is applied twice.
 */
struct depth_event {
	sequence_range sequence;       ///< Venue sequence numbers covered.
	timestamp event_time{};        ///< Venue event time, ns since epoch.
	std::vector<book_level> bids;  ///< Changed bid levels, absolute size.
	std::vector<book_level> asks;  ///< Changed ask levels, absolute size.
};

/**
 * @brief A normalised full-depth snapshot — the seed a diff feed is replayed
 *        onto.
 *
 * @c sequence is the last sequence number the snapshot already includes, so the
 * first diff that may be applied on top is the one covering @c sequence + 1.
 */
struct book_snapshot {
	std::uint64_t sequence = 0;   ///< Last sequence number included.
	timestamp event_time{};       ///< When the venue built it, ns since epoch.
	std::vector<book_level> bids; ///< Complete bid depth, any order.
	std::vector<book_level> asks; ///< Complete ask depth, any order.
};

/**
 * @brief Apply one normalised diff to @p book via absolute @c set_level writes.
 * @param book The book to mutate.
 * @param event The diff whose levels are set (size 0 removes the price).
 * @warning Sequencing is the caller's job: applying an event out of order
 *          silently corrupts the book. Route events through @c depth_sequencer
 *          (or @c depth_reconstructor, which owns both) rather than calling
 *          this on a raw feed.
 */
MARKET_DATA_EXPORT void apply(l2_book &book, const depth_event &event);

/**
 * @brief Replace @p book's contents with @p snapshot's depth.
 *
 * Both sides are installed wholesale through @c l2_book::load, so the venue's
 * level ordering does not matter and no per-level insert is paid.
 * @param book The book to reseed.
 * @param snapshot The full depth to install; read, not consumed — the book
 *        copies what fits into storage it already owns.
 */
MARKET_DATA_EXPORT void reset(l2_book &book, const book_snapshot &snapshot);

} // namespace exchange::market_data

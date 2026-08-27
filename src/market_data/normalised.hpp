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
//   * sequencing - one inclusive @ref inclusive_range per event, whatever the
//     venue calls its bounds;
//   * time - nanoseconds since the Unix epoch, whatever resolution it
//   publishes;
//   * levels - already scaled to the book's integral Price/Volume, in whatever
//     order they arrived (@c l2_book::load imposes the ordering).

#include "core/chrono/ingress.hpp" // ingress_time, has_ingress
#include "core/util/inclusive_range.hpp"
#include "fwd.hpp"                 // sequence_t,
#include "l2_book.hpp"
#include "market_data_export.hpp"
#include "orders/types.hpp" // IWYU pragma: keep - Price/Volume via book_level

#include <chrono>
#include <vector>

namespace exchange::market_data {

/// @brief A normalised aggregated level. Identical in shape to the book's own
///        cell, so a decoded side moves into an @c l2_book with no conversion.
using book_level = l2_book::price_level;

/// @brief Nanoseconds since the Unix epoch - the one time unit a normalised
///        feed speaks, whatever resolution the venue publishes.
using timestamp = std::chrono::nanoseconds;

/**
 * @brief One normalised depth diff: the levels a venue changed, plus the
 *        sequence range and time that place it in the feed.
 *
 * Each level is an @em absolute aggregate size, not a delta - a size of 0
 * removes the price. That is the L2 diff primitive @c l2_book::set_level takes,
 * and it is what makes replay idempotent for any event that is applied twice.
 */
struct depth_event {
	core::util::inclusive_range<sequence_t>
		sequence;           ///< Venue sequence numbers covered.
	timestamp event_time{}; ///< Venue event time, ns since epoch.
	/// @brief When this process received the frame this event was decoded from,
	///        or a default stamp if nothing stamped it. @see
	///        core::chrono::ingress_clock
	///
	/// Travels with the event rather than being tracked beside it, which is
	/// what makes the buffered-replay path correct for free: an event retained
	/// across a snapshot fetch is *moved* into @c depth_reconstructor's pending
	/// buffer and moved out again on replay, so it still reports when it
	/// arrived rather than when it was finally applied. That gap is the whole
	/// cost of a resync and is exactly what would go missing if the stamp were
	/// read at the point of application.
	///
	/// Default-initialised rather than left bare, for the reason
	/// @c feed_status::detail already states: without an initialiser GCC's
	/// -Wmissing-field-initializers fires at every designated initialiser that
	/// skips it, and skipping it is what "nothing stamped this" means. The @c
	/// {} is strictly redundant - a @c time_point's default constructor
	/// installs
	/// @c duration::zero() where a @c duration's leaves its rep indeterminate -
	/// so clang-tidy is told to allow it rather than the compiler told to.
	/// NOLINTNEXTLINE(readability-redundant-member-init)
	core::chrono::ingress_time ingress{};
	std::vector<book_level> bids{}; ///< Changed bid levels, absolute size.
	std::vector<book_level> asks{}; ///< Changed ask levels, absolute size.
};

/**
 * @brief A normalised full-depth snapshot - the seed a diff feed is replayed
 *        onto.
 *
 * @c sequence is the last sequence number the snapshot already includes, so the
 * first diff that may be applied on top is the one covering @c sequence + 1.
 */
struct book_snapshot {
	/// @brief Last sequence number included.
	///
	/// @c sequence_t, like @c depth_event::sequence: this value is handed
	/// straight to @c depth_sequencer::seed and compared against
	/// @c last_sequence(), so a different width here would make both a
	/// mixed-sign operation at the one place the feed decides whether a
	/// snapshot is newer than the book.
	sequence_t sequence = 0;
	timestamp event_time{}; ///< When the venue built it, ns since epoch.
	/// @brief When this process received the payload this was decoded from, or
	/// a
	///        default stamp if nothing stamped it. @see
	///        core::chrono::ingress_clock
	/// NOLINTNEXTLINE(readability-redundant-member-init)
	core::chrono::ingress_time ingress{};
	std::vector<book_level> bids{}; ///< Complete bid depth, any order.
	std::vector<book_level> asks{}; ///< Complete ask depth, any order.
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
 * @param snapshot The full depth to install; read, not consumed - the book
 *        copies what fits into storage it already owns.
 */
MARKET_DATA_EXPORT void reset(l2_book &book, const book_snapshot &snapshot);

} // namespace exchange::market_data

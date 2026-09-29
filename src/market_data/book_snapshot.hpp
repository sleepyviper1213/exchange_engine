#pragma once
// A normalised full-depth snapshot, and the one way to seed a book from it.
// @see normalised.hpp for what "normalised" fixes and why.

#include "core/chrono/ingress.hpp" // ingress_time
#include "fwd.hpp"                 // sequence_t
#include "l2_book.hpp"
#include "market_data_export.hpp"
#include "normalised.hpp"          // book_level, timestamp
#include "orders/types.hpp" // IWYU pragma: keep - Price/Volume via book_level

#include <vector>

namespace exchange::market_data {

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

#pragma once
// One normalised depth diff, and the one way to apply it to a book.
// @see normalised.hpp for what "normalised" fixes and why.

#include "core/chrono/ingress.hpp" // ingress_time, has_ingress
#include "core/util/inclusive_range.hpp"
#include "fwd.hpp"                 // sequence_t
#include "l2_book.hpp"
#include "market_data_export.hpp"
#include "normalised.hpp"          // book_level, timestamp
#include "orders/types.hpp" // IWYU pragma: keep - Price/Volume via book_level

#include <vector>

namespace exchange::market_data {

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
 * @brief Apply one normalised diff to @p book via absolute @c set_level writes.
 * @param book The book to mutate.
 * @param event The diff whose levels are set (size 0 removes the price).
 * @warning Sequencing is the caller's job: applying an event out of order
 *          silently corrupts the book. Route events through @c depth_sequencer
 *          (or @c depth_reconstructor, which owns both) rather than calling
 *          this on a raw feed.
 */
MARKET_DATA_EXPORT void apply(l2_book &book, const depth_event &event);

} // namespace exchange::market_data

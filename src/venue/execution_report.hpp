#pragma once
// What a venue says happened to an order we sent - normalised, and nothing
// about how it was spelled on the wire.
//
// The return leg of `outbound_order`. One goes out, an unbounded number of
// these come back for it: an acknowledgement, then a partial fill, then
// another, then a cancellation - each a separate message, each carrying the
// order's whole state as the venue currently understands it rather than a
// delta.
//
// --- why this is not `engine::OrderStatus` ---------------------------------
//
// Because the venue's lifecycle is not ours and pretending otherwise is how the
// two quietly diverge. A venue has states our book has no idea about
// (`PENDING_CANCEL`, `EXPIRED_IN_MATCH`), reports a rejection as a status
// rather than a refusal to accept, and can send a fill for an order whose
// acknowledgement has not arrived yet. Mapping that onto the engine's own
// lifecycle is a decision with policy in it, and it belongs where both
// vocabularies are in scope - `session/`, not here. This module's job is to say
// faithfully what the venue said.

#include "core/util/enum_string.hpp"
#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <cstdint>
#include <string>

namespace exchange::venue {

#define VENUE_EXECUTION_STATUS_LIST(X)                                         \
	X(accepted, "resting on the venue's book, unfilled")                       \
	X(partially_filled, "some quantity traded, the remainder still resting")   \
	X(filled, "fully traded; the order is done")                               \
	X(cancelled, "withdrawn before it filled, at our request or the venue's")  \
	X(pending_cancel, "a cancel is in flight; the order may still trade")      \
	X(rejected, "never accepted - the venue declined it outright")             \
	X(expired, "removed by the venue's own rules, not by us")                  \
	X(unknown, "a status this decoder does not recognise")

/**
 * @brief Where the venue believes the order stands.
 *
 * @note @c unknown is a value rather than a decode failure. A venue may add a
 *       status at any time, and refusing to decode the whole message because
 * one field grew a new enumerator would drop the fill it also carried. The
 *       caller sees a report it can still act on and a status it must not guess
 *       about.
 */
enum class execution_status : std::uint8_t {
	EXCHANGE_ENUM_VALUES(VENUE_EXECUTION_STATUS_LIST)
};

/// @brief The enumerator name of an @c execution_status, e.g. @c "filled".
/// @note Name rather than label: both macros emit @c format_as and a type may
///       have only one, and what a log wants here is the state's name. The
///       descriptions in the list above are documentation, which is all they
///       were ever read as.
EXCHANGE_ENUM_NAME(execution_status, to_string, VENUE_EXECUTION_STATUS_LIST)

#undef VENUE_EXECUTION_STATUS_LIST

#define VENUE_EXECUTION_KIND_LIST(X)                                           \
	X(acknowledgement, "the venue accepted the order; nothing traded")         \
	X(trade, "quantity traded on this message")                                \
	X(cancellation, "the order left the book without trading")                 \
	X(rejection, "the venue declined the order")                               \
	X(expiry, "the venue removed the order under its own rules")               \
	X(other, "an execution type this decoder does not recognise")

/**
 * @brief What this particular message reports, as opposed to where the order
 *        now stands.
 *
 * Separate from @c execution_status because the two answer different questions
 * and a caller needs both: a message with @c status ==
 * @c partially_filled and @c kind == @c trade carries new quantity to book,
 * while the same status with @c kind == @c cancellation is the *cancel* of an
 * order that had already partly filled and carries none. Reading one and
 * assuming the other is how a fill gets counted twice.
 */
enum class execution_kind : std::uint8_t {
	EXCHANGE_ENUM_VALUES(VENUE_EXECUTION_KIND_LIST)
};

/// @brief The enumerator name of an @c execution_kind, e.g. @c "trade".
/// @see execution_status::to_string for why this is the name and not the label.
EXCHANGE_ENUM_NAME(execution_kind, to_string, VENUE_EXECUTION_KIND_LIST)

#undef VENUE_EXECUTION_KIND_LIST

/**
 * @brief One report from a venue about one order.
 *
 * @note Prices and quantities are *scaled* integers in the listing's own units,
 *       decoded at the scales the caller supplies - the same contract
 *       @c market_data::binance::parse_binance_depth has, and for the same
 *       reason: a decimal becomes an integer exactly once, on a scale that has
 *       been chosen rather than guessed.
 */
struct execution_report {
	/// @brief The listing, as the venue spells it.
	std::string symbol{};

	/**
	 * @brief The identifier we sent as @c newClientOrderId.
	 *
	 * The reconciliation key, and the reason it is worth encoding the engine's
	 * own order id into it: this arrives on every report for the order,
	 * including ones that beat the placement's HTTP response back.
	 */
	std::string client_order_id{};

	/**
	 * @brief On a cancellation, the id of the order that was cancelled.
	 *
	 * Empty otherwise. The venue assigns the *cancel request* its own
	 * @c client_order_id and reports the cancelled order's here, so a caller
	 * that reads only @c client_order_id on a cancel report retires the wrong
	 * order - or no order at all.
	 */
	std::string original_client_order_id{};

	/// @brief The venue's own identifier for the order. Useful in a support
	///        conversation; not used as a key here, since it does not exist
	///        until the venue has replied at least once.
	std::int64_t venue_order_id = 0;

	execution_status status = execution_status::unknown;
	execution_kind kind     = execution_kind::other;

	/// @brief Quantity that traded on *this* message, scaled. Zero unless
	///        @c kind is @c trade.
	std::int64_t last_qty_scaled = 0;

	/// @brief Price that quantity traded at, scaled. Zero unless @c kind is
	///        @c trade - a venue sends 0 here on a non-trade report, and it
	///        means "not applicable" rather than "free".
	std::int64_t last_price_scaled = 0;

	/// @brief Total quantity traded across the order's whole life, scaled.
	///
	/// @note Cumulative, so it is the field to reconcile against rather than a
	///       running sum of @c last_qty_scaled - a report that arrives twice or
	///       out of order corrupts a sum and cannot corrupt this.
	std::int64_t cumulative_qty_scaled = 0;

	/// @brief The order's original size, scaled.
	std::int64_t order_qty_scaled = 0;

	/// @brief The order's limit price, scaled. Zero for a market order.
	std::int64_t order_price_scaled = 0;

	/// @brief Venue event time, milliseconds since the Unix epoch.
	std::int64_t event_time_ms = 0;

	/// @brief Venue transaction time - when the thing being reported actually
	///        happened, which is not when the message was emitted.
	std::int64_t transaction_time_ms = 0;

	/// @brief Whether our side provided liquidity on this trade. Meaningless
	///        unless @c kind is @c trade.
	bool is_maker = false;

	/// @brief The venue's reason for a rejection, verbatim. Empty otherwise.
	std::string reject_reason{};

	/// @brief Whether this message carries quantity that has just traded.
	[[nodiscard]] bool has_fill() const noexcept {
		return kind == execution_kind::trade && last_qty_scaled > 0;
	}

	/// @brief Whether the order is finished - nothing more will arrive for it.
	/// @note @c pending_cancel is deliberately *not* terminal: the order can
	///       still trade while the cancel is in flight, which is the race that
	///       makes cancel-after-fill the classic exchange bug.
	[[nodiscard]] bool is_terminal() const noexcept {
		return status == execution_status::filled ||
			   status == execution_status::cancelled ||
			   status == execution_status::rejected ||
			   status == execution_status::expired;
	}

	/// @brief The order this report is about - the cancelled order's id on a
	///        cancellation, ours otherwise.
	[[nodiscard]] const std::string &subject_order_id() const noexcept {
		return original_client_order_id.empty() ? client_order_id
												: original_client_order_id;
	}
};

} // namespace exchange::venue

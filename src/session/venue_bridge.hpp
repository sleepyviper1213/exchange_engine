#pragma once
// The single join between the engine's vocabulary and a venue's.
//
// `strategy/backtest/depth_feed_bridge.hpp` is this file's opposite number: it
// turns a venue's published depth into engine commands, and sits with its only
// caller so the edge runs downward. This does the same for the *order* path, in
// both directions - a command becomes something to send, an execution report
// becomes an outcome the engine's own consumers already understand - and it
// lives here for the same reason.
//
// --- what this deliberately does not produce -------------------------------
//
// An `engine::trade`. A trade names an aggressor and a resting order, and a
// venue tells us only about *our* side: there is no counterparty id to put in
// the other field, and inventing one would put a fabricated order id onto the
// event stream that persistence journals and the risk gate reads. A venue fill
// is reported as an `order_outcome` of type FILL, which carries everything the
// venue actually told us - cumulative traded, remaining, resulting status - and
// claims nothing it did not.
//
// --- and what it does not decide -------------------------------------------
//
// Whether to send. The gate has already run by the time a command reaches here;
// this is translation, and a translation layer that quietly declines to
// translate is a risk control nobody can find. Every refusal below is a failure
// to *express* the command in the venue's terms, never a judgement about it.


#include "order_book/outcome.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"
#include "session_export.hpp"
#include "symbol/symbol_spec.hpp"
#include "venue/execution_report.hpp"
#include "venue/outbound_order.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exchange::session {

#define VENUE_BRIDGE_ERROR_LIST(X)                                             \
	X(not_an_order, "the command is not a placement or a cancellation")        \
	X(unknown_order, "the report names an order this process did not place")   \
	X(price_off_grid, "the venue's price is not on the listing's tick grid")   \
	X(quantity_off_grid, "the venue's size is not on the listing's lot grid")  \
	X(out_of_range, "a venue quantity exceeds what the engine can represent")

/// @brief Why a command or a report could not be carried across the seam.
enum class bridge_error : std::uint8_t {
	EXCHANGE_ENUM_VALUES(VENUE_BRIDGE_ERROR_LIST)
};

/// @brief The category message for a @c bridge_error.
EXCHANGE_ENUM_LABEL(bridge_error, message, VENUE_BRIDGE_ERROR_LIST)

#undef VENUE_BRIDGE_ERROR_LIST

/**
 * @brief Translate @p order into something a venue gateway can send.
 *
 * @param order The engine's order, in ticks and lots.
 * @param spec The listing those ticks and lots are on.
 * @param venue_symbol The listing as the venue spells it (e.g. @c SOLUSDT),
 *        which is not @c spec.symbol() in general - the engine's symbol table
 *        and a venue's listing names are separate namespaces.
 * @return The outbound order, or why it could not be expressed.
 *
 * @note This is where ticks stop. @c symbol_spec turns them back into scaled
 *       integers and @c venue/ turns those into decimal text, so the conversion
 *       out of the engine's grid happens exactly once, here, where the grid is
 *       in scope. @see venue::outbound_order
 */
[[nodiscard]] SESSION_EXPORT std::expected<venue::outbound_order, bridge_error>
to_outbound_order(const engine::orders::order &order,
				  const engine::symbol_spec &spec,
				  std::string_view venue_symbol);

/// @brief Translate a cancellation of @p id on @p venue_symbol.
[[nodiscard]] SESSION_EXPORT venue::outbound_cancel
to_outbound_cancel(order_id_t id, std::string_view venue_symbol);

/**
 * @brief The engine status a venue status corresponds to.
 *
 * @note @c pending_cancel maps to @c PARTIALLY_FILLED or @c LIVE rather than to
 *       anything cancelled, because it is not: the order is still working and
 *       can still trade while the cancel is in flight. Treating it as terminal
 *       is precisely the cancel-after-fill bug, seen from the other side.
 * @note @c unknown maps to @c LIVE - the safe direction. Believing an order is
 *       still working when it is not costs a redundant cancel; believing it is
 *       gone when it is working leaves an unmanaged position on the venue.
 */
[[nodiscard]] constexpr engine::OrderStatus
to_engine_status(venue::execution_status status, bool any_filled) noexcept {
	using venue::execution_status;
	switch (status) {
	case execution_status::accepted:
		return any_filled ? engine::OrderStatus::PARTIALLY_FILLED
						  : engine::OrderStatus::LIVE;
	case execution_status::partially_filled:
		return engine::OrderStatus::PARTIALLY_FILLED;
	case execution_status::filled: return engine::OrderStatus::FILLED;
	case execution_status::cancelled: return engine::OrderStatus::CANCELLED;
	case execution_status::expired: return engine::OrderStatus::CANCELLED;
	case execution_status::rejected: return engine::OrderStatus::REJECTED;
	case execution_status::pending_cancel:
	case execution_status::unknown:
		return any_filled ? engine::OrderStatus::PARTIALLY_FILLED
						  : engine::OrderStatus::LIVE;
	default: return engine::OrderStatus::LIVE;
	}
}

/// @brief The outcome transition a venue execution kind corresponds to.
[[nodiscard]] constexpr engine::OutcomeType
to_outcome_type(venue::execution_kind kind) noexcept {
	using venue::execution_kind;
	switch (kind) {
	case execution_kind::acknowledgement: return engine::OutcomeType::ACCEPTED;
	case execution_kind::trade: return engine::OutcomeType::FILL;
	case execution_kind::cancellation: return engine::OutcomeType::CANCELLED;
	case execution_kind::expiry: return engine::OutcomeType::CANCELLED;
	case execution_kind::rejection: return engine::OutcomeType::REJECTED;
	case execution_kind::other: return engine::OutcomeType::ACCEPTED;
	default: return engine::OutcomeType::ACCEPTED;
	}
}

/**
 * @brief @p scaled as a lot count on @p spec's grid.
 *
 * @note Zero short-circuits rather than going through @c quantity_from_scaled,
 *       which refuses a non-positive size - correctly, for an order, since one
 *       cannot be placed for nothing. A *report* carries zero routinely: an
 *       acknowledgement and a rejection have both traded nothing, and reading
 *       that as an off-grid quantity would drop the two most common reports
 *       the venue sends.
 */
[[nodiscard]] SESSION_EXPORT std::expected<quantity_t, bridge_error>
lots_from(std::int64_t scaled, const engine::symbol_spec &spec);

/**
 * @brief Translate @p report into the outcome the engine's consumers read.
 *
 * @param report What the venue said.
 * @param spec The listing, for converting its scaled numbers back to lots.
 * @return The outcome, or why the report could not be used.
 *
 * @note Reads @c report.subject_order_id(), not @c client_order_id: on a
 *       cancellation those differ, and the one that names the order that went
 *       away is the former. @see venue::execution_report
 * @note @c traded is the venue's *cumulative* figure converted, never a running
 *       sum of the per-message quantities. A report delivered twice or out of
 *       order corrupts a sum and cannot corrupt this.
 */
[[nodiscard]] SESSION_EXPORT std::expected<engine::order_outcome, bridge_error>
to_outcome(const venue::execution_report &report,
		   const engine::symbol_spec &spec);
/**
 * @brief What a reconciliation found about one order.
 *
 * @see reconcile, which is where the three cases are argued.
 */
enum class reconciliation : std::uint8_t {
	agreed,        ///< both sides believe it is working
	adopt,         ///< the venue has it working and we do not
	presumed_gone, ///< we have it working and the venue does not
	foreign,       ///< working on the venue, but not placed by this process
};

/// @brief One order and what reconciling it concluded.
struct reconciled_order {
	/// @brief The client order id as the venue spells it. Kept as the venue's
	///        string because a @c foreign order has no engine id to name it by.
	std::string client_order_id{};

	/// @brief The engine id, when this is one of ours. Zero for @c foreign.
	order_id_t id = 0;

	reconciliation finding = reconciliation::agreed;
};

/**
 * @brief Compare what we believe is working against what the venue reports.
 *
 * The read that has to happen at startup and after any gap in the user data
 * stream, because those are exactly the windows in which a report can be lost.
 * An unanswered placement is the sharpest case: @c may_resend refuses to send
 * it again, precisely so that this can establish what it actually did.
 *
 * @param ours Engine ids this process believes are working.
 * @param venue_open Client order ids the venue reports as open.
 * @return One entry per order on either side, never silently dropped.
 *
 * @note A @c foreign order is reported and never actioned. The account may be
 *       shared with a human, and cancelling an order it did not recognise is
 *       the most destructive thing a reconciliation could do. @see
 *       CLIENT_ORDER_PREFIX
 * @note @c presumed_gone is a finding, not a conclusion. The order may have
 *       filled, been cancelled, or expired, and the venue's open-orders list
 *       cannot say which - only that it is not working now. What to tell the
 *       engine is the caller's decision.
 */
[[nodiscard]] SESSION_EXPORT std::vector<reconciled_order>
reconcile(std::span<const order_id_t> ours,
		  std::span<const std::string> venue_open);

} // namespace exchange::session

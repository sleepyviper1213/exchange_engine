#pragma once
// An order on its way *out*, as a venue would understand it.
//
// Named for its direction because the tree already has an `order_request`:
// `engine::order_request` in symbol/validation.hpp is the *inbound* one - a
// client's order as decimal text, before validation, possibly off the tick grid
// entirely. This is its opposite number: already validated, already scaled, on
// its way to a venue. Two types called `order_request` differing only by
// namespace is a name collision waiting for a using-directive, and it found
// one.
//
// Prices and sizes arrive here already *scaled* - integers in 10^-scale units,
// which is what `symbol_spec::price_to_scaled` hands back - rather than in the
// engine's ticks and lots. That is the seam: ticks are a fact about our book
// and the grid it was built on, and converting out of them needs the
// `symbol_spec` that defined them. Doing it before this type means `venue/`
// needs no edge to `symbol/`, and the one place that knows the grid does the
// one conversion that depends on it.

#include "orders/order_type.hpp"
#include "orders/side.hpp"
#include "orders/time_in_force_instruction.hpp"
#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <cstdint>
#include <string>

namespace exchange::venue {

/**
 * @brief One order to send, venue-agnostic.
 *
 * @note @c client_order_id is ours and comes back on every execution report
 *       for it, which is what makes reconciliation possible without a side
 *       table: encode the engine's @c order_id_t into it and a restarted
 *       process can still recognise its own working orders.
 */
struct outbound_order {
	/// @brief The listing, as the venue spells it (e.g. @c SOLUSDT).
	std::string symbol{};

	/// @brief Our identifier for this order, echoed back by the venue.
	std::string client_order_id{};

	side_t side = side_t::bid;

	/// @brief Limit or market. @c STOP is not sendable - nothing in this tree
	///        watches a trigger, and a venue-side stop is a different product.
	engine::orders::order_type type = engine::orders::order_type::LIMIT;

	/// @brief Duration. Only GTC, IOC and FOK map onto a venue;
	///        @c ALL_OR_NONE does not and is refused at encode time.
	engine::orders::time_in_force_instruction tif =
		engine::orders::time_in_force_instruction::GOOD_TILL_CANCELLED;

	/// @brief Limit price in @c 10^-price_scale units. Ignored for a market
	///        order, which must not carry one.
	std::int64_t price_scaled = 0;

	/// @brief Fractional digits the venue publishes prices at, for this
	///        listing. @see symbol_filters::price_decimals
	int price_scale = 0;

	/// @brief Size in @c 10^-qty_scale units. Must be positive.
	std::int64_t qty_scaled = 0;

	/// @brief Fractional digits for sizes. Frequently *not* equal to
	///        @c price_scale - SOLUSDT is 2 and 3, BTCUSDT 2 and 5.
	int qty_scale = 0;

	/// @brief Whether this order names a price at all.
	[[nodiscard]] bool has_price() const noexcept {
		return type == engine::orders::order_type::LIMIT;
	}
};

/// @brief An order to withdraw, named the way it was sent. @see outbound_order
struct outbound_cancel {
	std::string symbol{};

	/// @brief The @c client_order_id of the order to cancel. Cancelling by our
	///        own id rather than the venue's means a cancel can be issued
	///        before the placement's ack has come back, which is exactly when
	///        a strategy most wants to.
	std::string client_order_id{};
};

} // namespace exchange::venue

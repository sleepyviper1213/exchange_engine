#pragma once
#include "core/types.hpp"
#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine {

#define ORDER_TYPE_LIST(X)                                                     \
	X(MARKET, "take whatever the book offers; no price limit")                 \
	X(LIMIT, "trade only at the order's price or better")                      \
	X(STOP, "dormant until the market trades through the trigger price")

/**
 * @brief What price an order is willing to trade at.
 *
 * The *price* half of an order's instructions, and orthogonal to the *duration*
 * half in @c time_in_force_instruction: "limit" and "immediate or cancel" are
 * answers to different questions, and an order carries one of each. Splitting
 * them is what lets a marketable-limit IOC be expressed without a combinatorial
 * enumerator per pairing.
 *
 * @note @c STOP is declared but not matched: nothing in @c order_book watches a
 *       trigger price yet, so one is refused with @c UNSUPPORTED_ORDER_TYPE
 *       rather than rested like a limit — a stop that becomes live the instant
 *       it arrives is the opposite of what was asked for, and doing it silently
 *       is worse than declining. The enumerator and @c order::stop_price exist
 *       so the trigger machinery has somewhere to grow into.
 */
enum class order_type : std::uint8_t { EXCHANGE_ENUM_VALUES(ORDER_TYPE_LIST) };

/// @brief The enumerator name of an @c order_type, e.g. @c "LIMIT" (empty view
///        if out of range).
EXCHANGE_ENUM_NAME(order_type, to_string, ORDER_TYPE_LIST)

#define TIME_IN_FORCE_INSTRUCTION_LIST(X)                                      \
	X(GOOD_TILL_CANCELLED, "rest the unfilled remainder indefinitely")         \
	X(FILL_OR_KILL, "execute fully and immediately, or not at all")            \
	X(IMMEDIATE_OR_CANCEL, "execute what crosses now, drop the remainder")

/**
 * @brief How long an order may live — the duration half of its instructions.
 *
 * Decides only what becomes of the quantity that did not cross:
 * @c GOOD_TILL_CANCELLED rests it, @c IMMEDIATE_OR_CANCEL withdraws it as a
 * CANCELLED outcome carrying @c TIME_IN_FORCE, and @c FILL_OR_KILL refuses the
 * whole order up front unless the book can fill it entirely, so it never has a
 * remainder to decide about.
 */
enum class time_in_force_instruction : std::uint8_t {
	EXCHANGE_ENUM_VALUES(TIME_IN_FORCE_INSTRUCTION_LIST)
};

/// @brief The enumerator name of a @c time_in_force_instruction, e.g.
///        @c "FILL_OR_KILL" (empty view if out of range).
EXCHANGE_ENUM_NAME(time_in_force_instruction, to_string,
				   TIME_IN_FORCE_INSTRUCTION_LIST)

/**
 * @brief A validated order, on the engine's integer grid, ready to match.
 *
 * The output of the validation stage, never a client's own words: an
 * @c order_request carries decimal text and may name a price off the tick grid,
 * a quantity off the lot grid, or a symbol nobody lists, and @c validate turns
 * one into one of these or into a @c reject_reason. By the time an @c order
 * exists, @c price is a whole number of ticks and @c qty a whole number of
 * lots, both already checked against the listing's @c symbol_spec. Nothing
 * downstream re-derives that, so nothing downstream should construct one from
 * unvalidated input.
 *
 * Required fields come first so designated initialisers stay terse:
 * @code
 * book.place_order({.id = 1, .side = side_t::bid, .price = 100, .qty = 10},
 *                  trades, outcomes);
 * @endcode
 * @c type and @c timestamp default.
 */
struct order {
	/**
	 * @brief Client-assigned identifier, unique among resting orders.
	 *
	 * The key the id→location index uses, so it is what @c cancel_order needs
	 * and what every @c OrderOutcome names. Placing a second order under an id
	 * that is already resting is refused with @c DUPLICATE_ORDER_ID rather than
	 * accepted, because accepting it would orphan the first order's node.
	 *
	 * @note Zero is reserved. @c order_book treats it as the anonymous
	 * sentinel: such an order rests and matches normally but is not indexed,
	 * cannot be cancelled by id, and produces no outcomes. It is what
	 *       @c add_order uses to seed liquidity nobody owns.
	 */
	order_id_t id;

	/// @brief Which side of the book this order joins, and therefore which side
	///        it crosses against — @c opposed(side).
	side_t side;

	/**
	 * @brief What price the order will trade at. @see order_type
	 *
	 * Defaults to @c LIMIT because @c price is not optional: an order carrying
	 * a price that its type tells the book to ignore is a contradiction, and a
	 * defaulted field should be the conservative reading. An unpriced sweep has
	 * to be asked for.
	 */
	order_type type = order_type::LIMIT;

	/// @brief How long the order may live, and what becomes of the quantity
	/// that does not cross.
	time_in_force_instruction tif =
		time_in_force_instruction::GOOD_TILL_CANCELLED;

	/**
	 * @brief Limit price, as a whole number of the symbol's ticks.
	 *
	 * Ticks, not currency: @c symbol_spec::price_from_text did the conversion
	 * once, at the edge, and refused anything that was not an exact multiple.
	 * That is what lets prices compare and sort as plain integers here, with no
	 * rounding and no epsilon anywhere on the matching path.
	 *
	 * @note Read even when @c type is @c MARKET, since the field is not
	 *       optional; a market order should carry a price the book cannot
	 *       reject it on.
	 */
	price_t price;

	/**
	 * @brief Trigger price for a @c STOP order, in ticks. Zero means none.
	 *
	 * A stop order carries two prices and they do different jobs: @c stop_price
	 * is the level the market must trade through before the order exists as far
	 * as the book is concerned, and @c price is the limit it takes on once it
	 * does. Collapsing them into one field is the mistake that makes stop-limit
	 * unrepresentable — @c STOP with a @c stop_price equal to @c price is a
	 * stop-market in all but name, and the two are different orders.
	 *
	 * Zero is the "not a stop" sentinel rather than an @c std::optional: a
	 * price of 0 ticks is never admissible (a collar's floor is at least one
	 * tick), so the sentinel costs no representable state, and @c order has to
	 * stay trivially copyable for @c event::command's memcpy path.
	 *
	 * @note Validation enforces the pairing both ways — a @c STOP without a
	 *       trigger is @c MISSING_STOP_PRICE, and any other type carrying one
	 *       is @c UNEXPECTED_STOP_PRICE. When present it is held to the same
	 *       tick grid and collar as @c price.
	 * @warning Nothing triggers on it yet. @c order_book refuses @c STOP with
	 *          @c UNSUPPORTED_ORDER_TYPE rather than resting it like a limit,
	 *          because a stop that rests immediately is not a stop.
	 */
	price_t stop_price = 0;

	/**
	 * @brief Order quantity, as a whole number of the symbol's lots.
	 *
	 * Validated positive before the order is built — @c order_state has no
	 * representation for a non-positive order, and @c place_order refuses one
	 * with @c NON_POSITIVE_QUANTITY.
	 *
	 * @note Signed, because @c quantity_t is shared with the L2 diff feed,
	 * which expresses reductions as negatives. Nothing on this path wants that;
	 *       the matcher only ever sees values the validation boundary has
	 *       already proven positive.
	 */
	quantity_t qty;

	/**
	 * @brief Venue receipt time, nanoseconds since the Unix epoch.
	 *
	 * Carried end to end so a fill can be attributed in time, but not yet read
	 * by the matcher: time priority comes from FIFO position within a level,
	 * not from comparing this field, so a wrong or absent timestamp cannot
	 * currently change who fills first. Zero means "not stamped".
	 */
	std::uint64_t timestamp = 0;

	/// @brief Whether this order joins the bid side.
	[[nodiscard]] bool is_buy() const noexcept;

	/// @brief Whether any quantity remains to be filled.
	[[nodiscard]] bool has_quantity() const noexcept;

	/**
	 * @brief Deduct @p v from the order's quantity.
	 *
	 * @warning Unchecked — it will take the quantity negative. An order that is
	 * actually being filled belongs in an @c order_state, which refuses an
	 * overfill instead of wrapping past zero.
	 */
	void decrease_volume_by(quantity_t v) noexcept;

	/// @brief Field-by-field equality, timestamp included.
	bool operator==(const order &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<order>);
} // namespace exchange::engine

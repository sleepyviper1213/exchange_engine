#pragma once
#include "trading_engine_export.hpp" // TRADING_ENGINE_EXPORT (generated)
#include "trading-engine/orders/types.hpp"
#include "fwd.hpp"
#include "symbol_spec.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/order_book/reject_reason.hpp"

#include <expected>
#include <string_view>
#include <unordered_map>

namespace exchange::engine {

/**
 * @brief An order as a client sent it: decimal text, before any conversion.
 *
 * Deliberately not an @c Order. The point of the validation stage is that these
 * are different types with different guarantees - an @c order_request may carry
 * a price off the tick grid, a quantity of "0.0001" on a whole-lot listing, or
 * a symbol nobody lists. An @c Order may not. Making them one type with a
 * "validated" flag would put the check somewhere it can be forgotten.
 *
 * @note The string views must outlive the @c validate call, and no longer:
 *       validation converts them to integers immediately, and the resulting
 *       @c Order owns nothing.
 */
struct order_request {
	order_id_t id;
	symbol_id_t symbol;
	side_t side;
	std::string_view price;    ///< decimal text, e.g. "153.45"
	std::string_view quantity; ///< decimal text, e.g. "2.5"
	/// @brief Trigger price for a STOP order, as decimal text. Empty for every
	///        other type - supplying one anyway is a rejection, not a hint.
	std::string_view stop_price{};
	orders::order_type type = orders::order_type::LIMIT;
	orders::time_in_force_instruction tif =
		orders::time_in_force_instruction::GOOD_TILL_CANCELLED;
	std::uint64_t timestamp = 0;
};

/**
 * @brief Convert @p request onto @p spec's integer grid, or say why it cannot.
 *
 * The validation stage the architecture doc describes and the tree did not
 * have: it runs once, between parsing and dispatch, and it is the only place
 * decimal becomes integer. Everything downstream - the matching engine, the
 * book, the outcome stream - deals exclusively in ticks and lots and never
 * sees a decimal again.
 *
 * Checks, in the order a client would want them reported:
 * 1. price is well-formed decimal text the listing's scale can represent
 * 2. price is an exact multiple of the tick - @c PRICE_NOT_ON_TICK
 * 3. price is inside the collar - @c PRICE_OUTSIDE_COLLAR
 * 4. quantity is well-formed and positive
 * 5. quantity is an exact multiple of the lot - @c QUANTITY_NOT_ON_LOT
 *
 * @return The order on the engine's grid, or the first reason it was refused.
 *         The caller turns that reason into an @c order_outcome::rejected, so a
 *         refusal reaches the client on the same stream as a fill.
 */
[[nodiscard]] TRADING_ENGINE_EXPORT std::expected<orders::order, reject_reason>
validate(const order_request &request, const symbol_spec &spec) noexcept;

/**
 * @brief The listings the engine will trade, keyed by symbol id.
 *
 * A thin owner rather than a service: reference data is loaded once at startup
 * and read on every order, so the only operations that matter are "add a
 * listing" (never on the order path) and "find one" (always on it). The book
 * manager will hold one of these next to its per-symbol books; until it exists,
 * this is where a spec lives.
 */
struct symbol_registry {
	std::unordered_map<symbol_id_t, symbol_spec> by_id;

	/// @brief Register @p spec, replacing any listing under the same id.
	TRADING_ENGINE_EXPORT void add(symbol_spec spec);

	/// @brief The listing for @p id, or nullptr if there is none.
	[[nodiscard]] TRADING_ENGINE_EXPORT const symbol_spec *
	find(symbol_id_t id) const noexcept;

	/// @brief Look the symbol up and validate against it in one step.
	/// @return @c UNKNOWN_SYMBOL if @p request names a listing we do not have.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::expected<orders::order, reject_reason>
	validate(const order_request &request) const noexcept;
};

} // namespace exchange::engine

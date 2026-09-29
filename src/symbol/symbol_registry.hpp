#pragma once
// The listings the engine will trade, and the lookup that precedes validation.
// @see order_request.hpp for what validation itself does.

#include "fwd.hpp"
#include "order_book/reject_reason.hpp"
#include "order_request.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"
#include "symbol_export.hpp" // SYMBOL_EXPORT (generated)
#include "symbol_spec.hpp"

#include <expected>
#include <unordered_map>

namespace exchange::engine {

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
	SYMBOL_EXPORT void add(symbol_spec spec);

	/// @brief The listing for @p id, or nullptr if there is none.
	[[nodiscard]] SYMBOL_EXPORT const symbol_spec *
	find(symbol_id_t id) const noexcept;

	/// @brief Look the symbol up and validate against it in one step.
	/// @return @c UNKNOWN_SYMBOL if @p request names a listing we do not have.
	[[nodiscard]] SYMBOL_EXPORT std::expected<orders::order, reject_reason>
	validate(const order_request &request) const noexcept;
};

} // namespace exchange::engine

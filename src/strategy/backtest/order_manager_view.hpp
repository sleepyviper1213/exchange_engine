#pragma once
// The offline resting-order source: the venue's own record store, read directly.

#include "resting_source.hpp"

#include "execution/order_manager.hpp"
#include "orders/types.hpp"

#include <optional>

namespace exchange::strategy::backtest {

/**
 * @brief A @c resting_order_source over the partition's own record store.
 *
 * What the harness has always used, now spelled as the seam it always was. The
 * store is the matching engine's record rather than a copy of it, so there is no
 * second state machine here and nothing to drift: a price the model reads is the
 * price the book will match at, and a remaining quantity is what the last fill
 * left.
 *
 * @par Why this is only available offline
 * The record store belongs to the thread that runs the matching engine. A
 * single-threaded harness *is* that thread, so reading it costs nothing and
 * tells the truth. A live session is two threads and reading it from the
 * producer would be a data race - see @c session::ledger_view for what the
 * producer may read instead, and why the answer it gives is not merely a
 * substitute but a more honest one.
 *
 * @note Holds a pointer, so it is a view and must not outlive the store. Built
 *       per call at the point of use, which is what keeps that trivially true.
 */
class order_manager_view {
public:
	explicit order_manager_view(
		const engine::execution::order_manager &orders) noexcept
		: orders_(&orders) {}

	/// @brief @p id's side, price and remaining lots, or nothing if the venue
	///        has finished with it.
	[[nodiscard]] std::optional<resting_quote>
	resting(order_id_t id) const noexcept {
		const auto *record = orders_->find_record(id);
		if (record == nullptr) return std::nullopt;
		if (!is_active(*record)) return std::nullopt;
		const quantity_t left = record->state.remaining();
		if (left <= 0) return std::nullopt;
		return resting_quote{.side  = record->side,
							 .price = record->price,
							 .lots  = left};
	}

private:
	const engine::execution::order_manager *orders_;
};

static_assert(resting_order_source<order_manager_view>,
			  "the offline source must satisfy the concept it exists to model");

} // namespace exchange::strategy::backtest

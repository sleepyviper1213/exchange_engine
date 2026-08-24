#pragma once
// The live resting-order source: what the producer thread believes is working.

#include "risk_management/hooks/pre_trade/working_ledger.hpp"
#include "strategy/backtest/resting_source.hpp"

#include "orders/types.hpp"

#include <optional>

namespace exchange::session {

/**
 * @brief A @c resting_order_source over the risk gate's working-order ledger.
 *
 * @par Why the ledger, and not the record store
 * The fill model needs a side, a price and a remaining size for each order of
 * ours. Offline the harness reads them from @c order_manager, which is the
 * matching engine's own record. Live that store belongs to the consumer thread
 * and the producer may not touch it - so the question is what the producer
 * legitimately holds, and the answer already exists: the gate inserts every
 * order that clears its limits into @c working_ledger, draws it down from the
 * trade stream and retires it from the outcome stream. Same three fields, same
 * thread, no new state to maintain and nothing to keep in step.
 *
 * @par It does not track the book uniformly, and the direction matters
 * Worth being exact about, because it is easy to summarise this as "the ledger
 * lags the book" and that is only half true:
 *
 * - **It leads for placements.** The gate inserts at *screen* time, so an order
 *   is in the ledger from the moment it is accepted - before the consumer has
 *   applied it. The model may therefore infer a fill against an order still in
 *   flight and inject an aggressor for it. That aggressor reaches a book which
 *   does not hold the order, matches nothing, and is discarded, which is why
 *   @c crossing_fill_model::injected_lots is an upper bound rather than a count
 *   of fills. Nothing is double-counted; the model guessed early, exactly as a
 *   real strategy holding an unacknowledged order does.
 * - **It lags for fills and cancels.** Those arrive down the return path, so
 *   the ledger carries a quantity the book has already reduced until @c pump
 *   routes the outcome. The model can infer a second fill against lots that
 *   have gone - and again the aggressor finds nothing and dies.
 *
 * Both errors therefore fail the same safe way: they cost an injected order
 * that matches nothing. Neither can invent a fill, because a fill only exists
 * if the real book had the real order.
 *
 * @par Why this is still the right source, and not merely the available one
 * @c order_manager is an oracle. It reports a state change the instant the
 * matching engine makes it, which no strategy anywhere has ever known about its
 * own orders. The ledger is what the *producer* knows: what it has sent, and
 * what has come back. A model reading the oracle is measuring a strategy that
 * cannot be deployed. So the two sources will not agree exactly over the same
 * market, and the disagreement is information rather than error.
 *
 * @note The leading-for-placements behaviour is why @c live_session::inject runs
 *       before the quoter rather than after it. Read that function's note before
 *       reordering either.
 *
 * @note Holds a pointer, so it must not outlive the gate. Built per call at the
 *       point of use.
 */
class ledger_view {
public:
	explicit ledger_view(
		const risk::hooks::pre_trade::working_ledger &ledger) noexcept
		: ledger_(&ledger) {}

	/// @brief @p id's side, price and working lots, or nothing if the gate no
	///        longer counts it as exposure.
	[[nodiscard]] std::optional<strategy::backtest::resting_quote>
	resting(order_id_t id) const noexcept {
		const auto entry = ledger_->find(id);
		if (!entry.has_value()) return std::nullopt;
		if (entry->lots <= 0) return std::nullopt;
		return strategy::backtest::resting_quote{.side  = entry->side,
												 .price = entry->price,
												 .lots  = entry->lots};
	}

private:
	const risk::hooks::pre_trade::working_ledger *ledger_;
};

static_assert(strategy::backtest::resting_order_source<ledger_view>,
			  "the live source must satisfy the concept it exists to model");

} // namespace exchange::session

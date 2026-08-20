#pragma once
// The one piece of the live composition that is not the composition: what a
// single listing's published events are delivered to.
//
// `feedback_router` routes to *one* object per listing, and a live session has
// two that need the same span - the gate, whose state the next command is
// screened against, and the trader, which has to learn what became of its
// orders. So the fan-out is a type, and it goes in the router's slot rather
// than at the pump site: in a deployment carrying two listings the fan-out is
// per listing, and this is what keeps it that way.
//
// Templated on both halves rather than on a session, because it must not know
// what a session is - it is the thing a session hands to the router, and a test
// double for either half should be able to take its place.

#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstdint>
#include <span>

namespace exchange::app {

/**
 * @brief A fan-out recipient that wants nothing - the default third slot.
 *
 * Spelled as a type with empty hooks rather than detected with @c if constexpr,
 * because a fan-out's whole job is to call things: two branches per span to
 * discover that one of them is empty is worse than an inlined call to a
 * function with no body, which every compiler here deletes outright.
 */
struct no_watcher {
	void
	on_trades(std::span<const engine::trade> /*executions*/) const noexcept {}

	void on_outcomes(
		std::span<const engine::order_outcome> /*records*/) const noexcept {}
};

/**
 * @brief Delivers one listing's trades and outcomes to its gate and its trader.
 *
 * @tparam Gate The risk gate for the listing. Must offer @c symbol,
 *         @c on_trades, @c on_outcomes and @c working_orders - which is
 *         @c risk::hooks::watched_gate, spelled structurally here so this
 * header does not have to include the router to describe its own parameter.
 * @tparam Quoter The trader. Must offer @c on_trades and @c on_outcomes.
 * @tparam Watcher Anything else that wants the same two spans - a logger, a
 *         recorder, a metrics tap. Defaults to @c no_watcher, which compiles
 *         away. Called *last*, so whatever it observes is the state the gate
 * and the trader have already applied: a log line about a fill should describe
 * a position that has moved, not one that is about to.
 *
 * @note The gate is fed first, and the order is not cosmetic. The gate retires
 *       ledger entries and moves the position; the trader then decides what to
 *       do about a fill. A quoter that requoted off an outcome the gate had not
 *       yet accounted for would be sizing against a position that no longer
 *       existed.
 *
 * @note Both hooks are @c noexcept because the router's are, and the router's
 *       are because this sits on the return path: an exception here would
 *       escape through @c event_dispatcher's delivery loop, whose cursor is the
 *       only record of how much of a batch has been consumed.
 */
template <class Gate, class Quoter, class Watcher = no_watcher>
class feedback_fanout {
public:
	/// @param gate The listing's gate. Held by pointer; must outlive this.
	/// @param quoter The trader quoting it. Likewise.
	/// @param watcher Copied, so one that has to outlive the call carries a
	///        handle to whatever it reports to rather than the state itself -
	///        the same shape the gate's observer uses.
	feedback_fanout(Gate &gate, Quoter &quoter, Watcher watcher = {}) noexcept
		: gate_(&gate), quoter_(&quoter), watcher_(watcher) {}

	/// @brief The listing both halves belong to - what the router indexes by.
	[[nodiscard]] symbol_id_t symbol() const noexcept {
		return gate_->symbol();
	}

	void on_trades(std::span<const engine::trade> executions) noexcept {
		gate_->on_trades(executions);
		quoter_->on_trades(executions);
		watcher_.on_trades(executions);
	}

	void on_outcomes(std::span<const engine::order_outcome> records) noexcept {
		gate_->on_outcomes(records);
		quoter_->on_outcomes(records);
		watcher_.on_outcomes(records);
	}

	/// @brief What the gate believes is working - the reading the post-trade
	///        silence rule is about, and the only one it needs from a gate.
	///        @see risk::hooks::watched_gate
	[[nodiscard]] std::uint32_t working_orders() const noexcept {
		return gate_->working_orders();
	}

	/// @brief The watcher, for one that accumulates rather than forwards.
	[[nodiscard]] const Watcher &watcher() const noexcept { return watcher_; }

private:
	Gate *gate_;
	Quoter *quoter_;
	Watcher watcher_;
};

} // namespace exchange::app

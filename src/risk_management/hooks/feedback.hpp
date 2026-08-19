#pragma once
// The return path into risk: the adapter that takes a partition's published
// trades and outcomes and feeds each listing's back to the gate that screened
// it.
//
// A gate is only correct if something closes this loop. Its limits are measured
// against a position that moves when a fill prints and a working total that
// shrinks when an order dies, and `on_trades` / `on_outcomes` are how it learns
// either happened. A gate nobody feeds keeps every order it ever sent in its
// ledger, so its exposure ratchets closed over a session until it refuses
// everything - the failure is silent, gradual, and looks like a limit that is
// too tight.
//
// `event_dispatcher` already delivers events per listing, and its handler
// concept takes the symbol as a parameter precisely because "which consumer
// owns this listing" is a deployment's question rather than that module's. This
// is the risk side's answer to it, and it lives here rather than in `app/` for
// the same reason `depth_feed_bridge` lives beside its caller: the mapping is
// risk vocabulary - a gate per listing, one thread, dense symbol ids, exactly
// the shape `position_book` already indexes by.
//
// `strategy/backtest/session.hpp` does this inline for one gate, because a
// backtest holds one listing and drives the loop by hand. Nothing in the live
// topology did it at all before this, which is the gap.

#include "fwd.hpp"
#include "risk_management/hooks/pre_trade/position.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exchange::risk::hooks {

/**
 * @brief The half of a gate this adapter uses: which listing it screens, and
 * the two feedback hooks.
 *
 * A concept rather than @c risk_gate itself, so this header does not include
 * @c gate.hpp - a test double is three functions, and a deployment that stacks
 * two gates can route to either. The hooks are required @c noexcept because
 * this sits on the return path and the gate's are: an exception here would
 * escape through @c event_dispatcher's delivery loop, whose cursor is the only
 * record of how much of a batch has been consumed.
 */
template <class G>
concept feedback_gate =
	requires(G &gate, std::span<const engine::trade> executions,
			 std::span<const engine::order_outcome> records) {
		{ gate.symbol() } -> std::convertible_to<symbol_id_t>;
		{ gate.on_trades(executions) } noexcept;
		{ gate.on_outcomes(records) } noexcept;
	};

/**
 * @brief Routes published events to the gate that owns each listing.
 *
 * Models @c engine::event::event_handler, so it is what an @c event_dispatcher
 * is constructed with:
 *
 * @code
 * risk::hooks::feedback_router<gate_t> hooks{listings};
 * hooks.attach(btc_gate);
 * hooks.attach(eth_gate);
 *
 * event::event_dispatcher dispatch{channel, hooks};
 * while (running) dispatch.pump();
 * @endcode
 *
 * @tparam Gate What is being fed. @see feedback_gate
 *
 * @par Why the conformance is not asserted here
 * Naming @c event_handler means including @c event_dispatcher.hpp, which is the
 * whole dispatcher template, for a one-line check. The edge would be legal -
 * @c risk_management already depends on @c trading_engine - but the weight is
 * not worth it, so the @c static_assert sits in the test tree beside the one
 * that pins the @c command_sink conformance on the way in.
 *
 * @par It always consumes the whole span, and that is a contract not a shortcut
 * A handler's return value is back-pressure: @c event_dispatcher keeps whatever
 * a handler did not take and re-delivers it before dequeuing anything new. A
 * gate cannot refuse - it is updating its own state, not filling a queue - so
 * this returns the size it was given, always. Returning less would stall the
 * dispatcher on the same run forever, which is why an event for a listing no
 * gate screens is *counted and consumed* rather than left behind: silence about
 * an unknown symbol is a metric, but refusing it is a hang.
 *
 * @par Routing is not a nicety
 * Handing a listing's trades to another listing's gate would corrupt both: the
 * gate looks both order ids up in its own ledger, marks its fat-finger band at
 * the price, and values its exposure against it. A wrong-listing print
 * therefore moves a position that never traded and re-marks a band around a
 * price from another instrument. There is deliberately no fallback gate for
 * that reason.
 *
 * @par Threading
 * One hooks object, one thread - the dispatcher's consumer thread, which is the
 * thread every gate it routes to lives on and the same thread those gates'
 * hosts submit from. Nothing here synchronises because nothing here is shared;
 * @c event_channel already crossed the boundary.
 *
 * @par Allocation
 * One vector, sized at construction and never grown, indexed by symbol id the
 * way @c position_book is. A null entry is a listing this process screens no
 * orders for, which is the common case in a partition that carries more symbols
 * than one strategy trades.
 */
template <feedback_gate Gate>
class feedback_router {
public:
	/// @brief Listings a default set of hooks can route. Dense symbol ids index
	///        the table directly, so this is a highest-id bound and not a count
	///        - the same bound, and for the same reason, as the position book
	///        the gates share. @see position_book::DEFAULT_CAPACITY
	static constexpr std::size_t DEFAULT_LISTINGS =
		position_book::DEFAULT_CAPACITY;

	/// @param listings Highest symbol id, exclusive, that may be routed.
	explicit feedback_router(std::size_t listings = DEFAULT_LISTINGS)
		: gates_(listings, nullptr) {}

	/**
	 * @brief Route @p gate's listing to it. Deployment-time wiring, not
	 *        something an event path does.
	 *
	 * @pre @c gate.symbol() is inside @c capacity() and has no gate already. A
	 *      second gate for one listing is a wiring bug rather than a stacking
	 *      feature - two gates over one listing double-count every fill - and
	 *      stacked gates are stacked on the *submit* side, where the inner
	 * one's sink is the outer one. Only the innermost is fed from here.
	 * @note The gate is not owned and must outlive these hooks, like the sink a
	 *       gate itself holds.
	 */
	void attach(Gate &gate) {
		const auto slot = static_cast<std::size_t>(gate.symbol());
		assert(slot < gates_.size() &&
			   "the hooks must be sized for this listing");
		assert(gates_[slot] == nullptr &&
			   "one listing, one gate - see attach()");
		gates_[slot] = &gate;
		++listings_;
	}

	// --- what event_dispatcher calls --------------------------------------

	/// @brief Apply @p executions to the gate that screens @p symbol.
	/// @return @p executions.size(), always. @see the class note.
	std::size_t on_trades(symbol_id_t symbol,
						  std::span<const engine::trade> executions) noexcept {
		if (Gate *gate = gate_for(symbol)) {
			gate->on_trades(executions);
			applied_trades_ += executions.size();
		} else {
			unrouted_ += executions.size();
		}
		return executions.size();
	}

	/// @brief Apply @p records to the gate that screens @p symbol.
	/// @return @p records.size(), always. @see the class note.
	std::size_t
	on_outcomes(symbol_id_t symbol,
				std::span<const engine::order_outcome> records) noexcept {
		if (Gate *gate = gate_for(symbol)) {
			gate->on_outcomes(records);
			applied_outcomes_ += records.size();
		} else {
			unrouted_ += records.size();
		}
		return records.size();
	}

	// --- what an operator reads -------------------------------------------

	/// @brief The gate screening @p symbol, or @c nullptr if none is attached.
	[[nodiscard]] Gate *gate_for(symbol_id_t symbol) const noexcept {
		const auto slot = static_cast<std::size_t>(symbol);
		return slot < gates_.size() ? gates_[slot] : nullptr;
	}

	/// @brief Whether @p symbol is inside the routable range - not whether a
	///        gate is attached for it. @see gate_for
	[[nodiscard]] bool carries(symbol_id_t symbol) const noexcept {
		return static_cast<std::size_t>(symbol) < gates_.size();
	}

	/// @brief Highest symbol id, exclusive, these hooks can route.
	[[nodiscard]] std::size_t capacity() const noexcept {
		return gates_.size();
	}

	/// @brief Gates attached.
	[[nodiscard]] std::size_t listings() const noexcept { return listings_; }

	/// @brief Trades handed to a gate since construction.
	[[nodiscard]] std::uint64_t applied_trades() const noexcept {
		return applied_trades_;
	}

	/// @brief Lifecycle records handed to a gate since construction.
	[[nodiscard]] std::uint64_t applied_outcomes() const noexcept {
		return applied_outcomes_;
	}

	/**
	 * @brief Events consumed for a listing no gate screens.
	 *
	 * Not an error on its own: a partition publishes everything it matched, and
	 * a strategy that trades two of its ten listings will see the other eight
	 * arrive here. It *is* the number to look at when a gate's position looks
	 * stale, because the other reading of a rising count is a gate that was
	 * never attached.
	 */
	[[nodiscard]] std::uint64_t unrouted() const noexcept { return unrouted_; }

private:
	// Non-owning, and indexed by symbol id rather than searched: a gate is
	// neither copyable nor movable, so a pointer is the only handle there is,
	// and the ids a partition publishes are the same dense ones position_book
	// is already indexed by. A hash would buy nothing over an array indexed by
	// the key itself.
	std::vector<Gate *> gates_;
	std::size_t listings_ = 0;

	std::uint64_t applied_trades_   = 0;
	std::uint64_t applied_outcomes_ = 0;
	std::uint64_t unrouted_         = 0;
};

} // namespace exchange::risk::hooks

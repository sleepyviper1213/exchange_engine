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
//
// --- and why the post-trade lane is driven from here ----------------------
//
// Because this is already the one thing in the process that sees every event a
// partition published, in order, for every listing, on one thread. That is the
// entire input of `hooks/post_trade/` - so a second consumer of the same stream
// would be a second table indexed by the same symbol ids, a second clock read
// per span, and a composite handler to hand the dispatcher, in exchange for
// nothing.
//
// A monitor is therefore attached beside the gate that screens its listing and
// fed from the same two calls. The gate half and the monitor half stay strictly
// separate below: the gate is state the *next* command is screened against, and
// its update cannot be skipped; the monitor is surveillance, and a deployment
// that attaches none pays one null check per span for it.

#include "fwd.hpp"
#include "risk_management/clock.hpp"
#include "risk_management/hooks/post_trade/monitor.hpp"
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
 * @brief A gate that can also be watched: it can say how much it thinks is
 *        working.
 *
 * Split from @c feedback_gate rather than folded into it because the reading is
 * needed by exactly one rule - @c outcome_silence, which is the only one whose
 * subject is the absence of events - and requiring it of every gate would make
 * a three-function test double a four-function one for the benefit of a
 * deployment that attaches no monitor. The two @c attach overloads and @c poll
 * are constrained on this instead, so a gate that cannot answer simply has no
 * post-trade lane rather than failing to compile. @see post_trade_monitor::poll
 */
template <class G>
concept watched_gate = feedback_gate<G> && requires(const G &gate) {
	{ gate.working_orders() } -> std::convertible_to<std::uint32_t>;
};

/**
 * @brief Routes published events to the gate that owns each listing, and to the
 *        post-trade monitor watching it.
 *
 * Models @c engine::event::event_handler, so it is what an @c event_dispatcher
 * is constructed with:
 *
 * @code
 * risk::hooks::feedback_router<gate_t> hooks{listings};
 * hooks.attach(btc_gate, btc_watch);   // with post-trade surveillance
 * hooks.attach(eth_gate);              // without
 *
 * event::event_dispatcher dispatch{channel, hooks};
 * while (running) {
 *     dispatch.pump();
 *     hooks.poll();   // the one rule no arriving event can drive
 * }
 * @endcode
 *
 * @tparam Gate What is being fed. @see feedback_gate
 * @tparam Clock Where "now" comes from, for the post-trade lane's rules. Read
 *         once per delivered span and never per event, the same way
 *         @c risk_gate reads one per screened batch - and read only when a
 *         monitor is attached, because a deployment with none should not pay
 *         tens of nanoseconds per span for a reading nothing consumes. A
 *         replay needs recorded time here for the same reason the gate does.
 *         @see nanosecond_clock
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
template <feedback_gate Gate, nanosecond_clock Clock = steady_nanos>
class feedback_router {
public:
	/// @brief Listings a default set of hooks can route. Dense symbol ids index
	///        the table directly, so this is a highest-id bound and not a count
	///        - the same bound, and for the same reason, as the position book
	///        the gates share. @see position_book::DEFAULT_CAPACITY
	static constexpr std::size_t DEFAULT_LISTINGS =
		pre_trade::position_book::DEFAULT_CAPACITY;

	/// @param listings Highest symbol id, exclusive, that may be routed.
	/// @param clock Where "now" comes from for the post-trade lane.
	explicit feedback_router(std::size_t listings = DEFAULT_LISTINGS,
							 Clock clock          = {})
		: slots_(listings), clock_(clock) {}

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
		const auto id = static_cast<std::size_t>(gate.symbol());
		assert(id < slots_.size() &&
			   "the hooks must be sized for this listing");
		assert(slots_[id].gate == nullptr &&
			   "one listing, one gate - see attach()");
		slots_[id].gate = &gate;
		++listings_;
	}

	/**
	 * @brief Route @p gate's listing to it, and watch the same listing with
	 *        @p monitor.
	 *
	 * @pre Both name the same listing, and it has neither a gate nor a monitor
	 *      already. A monitor over one listing fed a different one's prints
	 *      would count executions the account never had and time a return path
	 *      that is not its own - the same corruption a misrouted gate suffers,
	 *      and refused for the same reason.
	 * @note Neither is owned, and both must outlive these hooks. This is the
	 *       one call here that allocates: the watched listings are kept in a
	 *       dense list so @c poll walks what is attached rather than the whole
	 *       symbol-id range. Deployment-time wiring, not something an event
	 *       path does.
	 */
	void attach(Gate &gate, post_trade::post_trade_monitor &monitor)
		requires watched_gate<Gate>
	{
		assert(gate.symbol() == monitor.symbol() &&
			   "a monitor watches the listing its gate screens");
		attach(gate);
		const auto id      = static_cast<std::size_t>(gate.symbol());
		slots_[id].monitor = &monitor;
		watched_.push_back(id);
	}

	// --- what event_dispatcher calls --------------------------------------

	/// @brief Apply @p executions to the gate that screens @p symbol, and to
	///        the monitor watching it.
	/// @return @p executions.size(), always. @see the class note.
	std::size_t on_trades(symbol_id_t symbol,
						  std::span<const engine::trade> executions) noexcept {
		entry *slot = entry_for(symbol);
		if (slot == nullptr || slot->gate == nullptr) {
			unrouted_ += executions.size();
			return executions.size();
		}
		slot->gate->on_trades(executions);
		if (slot->monitor != nullptr)
			slot->monitor->on_trades(executions, clock_.now_ns());
		applied_trades_ += executions.size();
		return executions.size();
	}

	/// @brief Apply @p records to the gate that screens @p symbol, and to the
	///        monitor watching it.
	/// @return @p records.size(), always. @see the class note.
	std::size_t
	on_outcomes(symbol_id_t symbol,
				std::span<const engine::order_outcome> records) noexcept {
		entry *slot = entry_for(symbol);
		if (slot == nullptr || slot->gate == nullptr) {
			unrouted_ += records.size();
			return records.size();
		}
		slot->gate->on_outcomes(records);
		if (slot->monitor != nullptr)
			slot->monitor->on_outcomes(records, clock_.now_ns());
		applied_outcomes_ += records.size();
		return records.size();
	}

	/**
	 * @brief Poll every attached monitor for the rules no arriving event can
	 *        drive.
	 *
	 * @return Monitors that tripped the breaker on this call. Zero is the
	 *         overwhelmingly common answer and the one worth being cheap: with
	 *         nothing watched this is a compare and a return, and it reads no
	 *         clock at all.
	 *
	 * Call it from whatever loop already pumps the dispatcher. Only
	 * @c outcome_silence needs it - its subject is the *absence* of events, and
	 * an absence delivers no callback - so a loop that never polls simply never
	 * fires that one rule. Stated here rather than left to be discovered,
	 * because a watchdog nobody winds is the failure it was built to catch.
	 *
	 * @note One clock read for the whole sweep, and the working count comes
	 *       from each gate rather than from the monitor: a monitor holding a
	 *       gate pointer would point an edge from the post-trade lane at
	 *       @c risk_gate for one @c std::uint32_t. @see watched_gate
	 */
	std::size_t poll() noexcept
		requires watched_gate<Gate>
	{
		if (watched_.empty()) return 0;
		const std::uint64_t now = clock_.now_ns();
		std::size_t tripped     = 0;
		for (const std::size_t id : watched_) {
			entry &slot = slots_[id];
			if (slot.monitor->poll(now, slot.gate->working_orders())) ++tripped;
		}
		return tripped;
	}

	// --- what an operator reads -------------------------------------------

	/// @brief The gate screening @p symbol, or @c nullptr if none is attached.
	[[nodiscard]] Gate *gate_for(symbol_id_t symbol) const noexcept {
		const entry *slot = entry_for(symbol);
		return slot != nullptr ? slot->gate : nullptr;
	}

	/// @brief The monitor watching @p symbol, or @c nullptr if none is
	///        attached. A listing can have a gate and no monitor; the reverse
	///        is refused by @c attach.
	[[nodiscard]] post_trade::post_trade_monitor *
	monitor_for(symbol_id_t symbol) const noexcept {
		const entry *slot = entry_for(symbol);
		return slot != nullptr ? slot->monitor : nullptr;
	}

	/// @brief Whether @p symbol is inside the routable range - not whether a
	///        gate is attached for it. @see gate_for
	[[nodiscard]] bool carries(symbol_id_t symbol) const noexcept {
		return static_cast<std::size_t>(symbol) < slots_.size();
	}

	/// @brief Highest symbol id, exclusive, these hooks can route.
	[[nodiscard]] std::size_t capacity() const noexcept {
		return slots_.size();
	}

	/// @brief Gates attached.
	[[nodiscard]] std::size_t listings() const noexcept { return listings_; }

	/// @brief Listings with a post-trade monitor attached - never more than
	///        @c listings().
	[[nodiscard]] std::size_t watched() const noexcept {
		return watched_.size();
	}

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
	/// @brief What one listing routes to. Two pointers rather than two tables:
	///        both are looked up by the same id on the same event, so they want
	///        to be on the same cache line.
	struct entry {
		Gate *gate                              = nullptr;
		post_trade::post_trade_monitor *monitor = nullptr;
	};

	[[nodiscard]] entry *entry_for(symbol_id_t symbol) noexcept {
		const auto id = static_cast<std::size_t>(symbol);
		return id < slots_.size() ? &slots_[id] : nullptr;
	}

	[[nodiscard]] const entry *entry_for(symbol_id_t symbol) const noexcept {
		const auto id = static_cast<std::size_t>(symbol);
		return id < slots_.size() ? &slots_[id] : nullptr;
	}

	// Non-owning, and indexed by symbol id rather than searched: a gate is
	// neither copyable nor movable, so a pointer is the only handle there is,
	// and the ids a partition publishes are the same dense ones position_book
	// is already indexed by. A hash would buy nothing over an array indexed by
	// the key itself.
	std::vector<entry> slots_;
	std::size_t listings_ = 0;

	// The ids in slots_ that have a monitor, densely. poll() walks this rather
	// than the whole table, so a process carrying two hundred listings and
	// watching two does not touch two hundred cache lines per pass of its event
	// loop.
	std::vector<std::size_t> watched_;

	Clock clock_;

	std::uint64_t applied_trades_   = 0;
	std::uint64_t applied_outcomes_ = 0;
	std::uint64_t unrouted_         = 0;
};

} // namespace exchange::risk::hooks

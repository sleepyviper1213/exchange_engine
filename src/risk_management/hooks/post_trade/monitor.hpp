#pragma once
// The one owner of a listing's post-trade rules, and the thing that feeds them.
//
// This is the lane's composer, and it exists where the other two lanes' do not
// for a reason worth stating up front. `pre_trade/` is composed by `risk_gate`,
// which calls every rule unconditionally and ORs the bits - the composition is
// branchless arithmetic and belongs on the submit path. `system/` is composed
// by nothing at all: its three hooks are driven by three unrelated signals and
// meet only in the `circuit_breaker` they all write, so a class that sequenced
// them would be inventing an order nobody needs.
//
// This lane has a single input - everything a partition published for one
// listing, in order - one thread, and a loop. That is an object. @see
// post_trade/fwd.hpp for why the lane is filed this way.

#include "risk_management/hooks/post_trade/fill_burst.hpp"
#include "risk_management/hooks/post_trade/fwd.hpp"
#include "risk_management/hooks/post_trade/limits.hpp"
#include "risk_management/hooks/post_trade/order_trade_ratio.hpp"
#include "risk_management/hooks/post_trade/outcome_silence.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstdint>
#include <span>

namespace exchange::risk::hooks::post_trade {

/**
 * @brief Watches one listing's published events and trips the shared breaker
 *        when the *shape* of them says something is wrong.
 *
 * @par How it gets driven
 * By @c hooks::feedback_router, which is already the thing that takes a
 * partition's published events and routes each listing's to the gate that
 * screened it. A monitor is attached beside that gate and sees the same two
 * spans:
 *
 * @code
 * namespace risk = exchange::risk;
 * risk::hooks::post_trade::post_trade_monitor watch{breaker, BTC, policy, now};
 *
 * risk::hooks::feedback_router<gate_t> hooks{listings};
 * hooks.attach(btc_gate, watch);
 *
 * event::event_dispatcher dispatch{channel, hooks};
 * while (running) {
 *     dispatch.pump();
 *     hooks.poll();     // the one rule an arriving event cannot drive
 * }
 * @endcode
 *
 * @par Where the time comes from, and why it is a parameter
 * The router reads its clock once per delivered span and passes the reading
 * down, the same way @c risk_gate reads one per screened batch. Every rule here
 * would otherwise read a clock of its own for events that arrived together, and
 * a @c now() is tens of nanoseconds - affordable on this thread, but there is
 * nothing to buy with it. It also keeps the whole lane drivable from recorded
 * time, which is what a replay needs. @see risk::clock.hpp
 *
 * @par Order of the two hooks, which is not arbitrary
 * A partition publishes a listing's trades before its outcomes, and this lane
 * relies on that only in one place: @c on_trades feeds the ratio's denominator,
 * so an execution counted before the messages that follow it makes the ratio
 * read *lower* during a batch than it will at the end of one. That is the
 * forgiving direction, and forgiving is right for a rule that trips a breaker.
 *
 * @par What is not here: an observer
 * @c risk_gate takes one because a refusal is an event with a subject - this
 * order, these rules - and a counter cannot name it. A trip has no subject and
 * is already published: it is a state change on @c circuit_breaker, carrying
 * the
 * @c trip_cause that says which rule fired, readable by everything that already
 * reads the breaker. A second notification channel for one bit that is already
 * atomic would be machinery for nothing. Counters here are per rule so an
 * operator can see how close the others were.
 *
 * @par Threading
 * One monitor, one listing, one thread - the dispatcher's consumer thread,
 * which is the thread the listing's gate lives on and the thread that submits
 * through it. Every counter in here is therefore plain, like every other
 * counter in this module; the *decision* crosses the boundary through @c
 * circuit_breaker, which is atomic precisely so that it can.
 *
 * @note The breaker's own @c trips() counter is not atomic, and it is written
 * by whichever thread trips. That is safe here because this lane shares a
 *       thread with the gate - see @c feedback_router's threading note - and it
 *       is the constraint to check first if a deployment ever moves post-trade
 *       monitoring onto a thread of its own.
 */
class post_trade_monitor {
public:
	/**
	 * @brief Watch @p symbol, tripping @p breaker per @p limits.
	 *
	 * @param breaker The shared kill switch. Must outlive the monitor.
	 * @param symbol The listing this monitor watches. Every event it is handed
	 *        must be that listing's - see @c feedback_router on why there is no
	 *        fallback.
	 * @param limits The thresholds. Copied, like @c risk_gate copies its own:
	 *        it is read on every event and wants to be in this object's cache
	 *        line rather than behind a pointer.
	 * @param now_ns Seeds the silence rule, which is the only one that measures
	 *        from construction rather than from an event.
	 */
	RISK_MANAGEMENT_EXPORT post_trade_monitor(system::circuit_breaker &breaker,
											  symbol_id_t symbol,
											  const post_trade_limits &limits,
											  std::uint64_t now_ns) noexcept;

	// Pinned to the thread that drives it, like the gate it sits beside.
	// Nothing here would break under a move; there is simply nowhere for one to
	// go, and allowing it would invite a monitor to be relocated out from under
	// the router holding a reference to it.
	post_trade_monitor(const post_trade_monitor &)            = delete;
	post_trade_monitor &operator=(const post_trade_monitor &) = delete;
	post_trade_monitor(post_trade_monitor &&)                 = delete;
	post_trade_monitor &operator=(post_trade_monitor &&)      = delete;
	~post_trade_monitor()                                     = default;

	// --- what the router calls ---------------------------------------------

	/**
	 * @brief Apply @p executions, as of @p now_ns.
	 *
	 * Each print feeds two rules: the burst counters, and the ratio's
	 * denominator. Nothing here looks an order id up - which side of a print
	 * was ours is the gate's question, and @c fill_burst says what that costs
	 * the rule.
	 *
	 * @return Whether any rule tripped the breaker on this span. Returned
	 * rather than swallowed so a test can pin the boundary; a router ignores it
	 *         because a trip is already published through the breaker.
	 */
	RISK_MANAGEMENT_EXPORT bool
	on_trades(std::span<const engine::trade> executions,
			  std::uint64_t now_ns) noexcept;

	/**
	 * @brief Apply @p records, as of @p now_ns.
	 *
	 * Every record is evidence the return path is alive, so every record beats
	 * the silence rule. Only those that represent a message the venue had to
	 * process count towards the ratio - @c is_venue_message owns that policy
	 * and explains the three places it differs from "one outcome, one message".
	 *
	 * @return Whether any rule tripped the breaker on this span.
	 */
	RISK_MANAGEMENT_EXPORT bool
	on_outcomes(std::span<const engine::order_outcome> records,
				std::uint64_t now_ns) noexcept;

	/**
	 * @brief Check the rules that no arriving event can drive.
	 *
	 * @param now_ns The current reading.
	 * @param working Orders the listing's gate believes are still out there.
	 * @return Whether *this call* tripped the breaker.
	 *
	 * Only @c outcome_silence needs this, because its subject is the absence of
	 * events and an absence delivers no callback. Call it from whatever loop
	 * already runs; a monitor nobody polls simply never fires that one rule.
	 */
	RISK_MANAGEMENT_EXPORT bool poll(std::uint64_t now_ns,
									 std::uint32_t working) noexcept;

	// --- what an operator reads --------------------------------------------

	/// @brief The listing this monitor watches.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT symbol_id_t symbol() const noexcept;

	/// @brief The thresholds in force.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT const post_trade_limits &
	limits() const noexcept;

	/// @brief The order-to-trade rule, for its counters.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT const order_trade_ratio &
	ratio() const noexcept;

	/// @brief The burst and run rule, for its counters.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT const fill_burst &
	fills() const noexcept;

	/// @brief The return-path watchdog, for its counters.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT const outcome_silence &
	silence() const noexcept;

	/// @brief Trips this monitor's three rules have caused between them - the
	///        number to look at before deciding which one to read.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t trips() const noexcept;

private:
	symbol_id_t symbol_;
	post_trade_limits limits_;
	order_trade_ratio ratio_;
	fill_burst fills_;
	outcome_silence silence_;
};

} // namespace exchange::risk::hooks::post_trade
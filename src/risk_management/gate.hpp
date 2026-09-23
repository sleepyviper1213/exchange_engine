#pragma once
// The pre-engine::trade check, sitting inline between a strategy host and the
// gateway.

#include "core/chrono/clock.hpp"
#include "core/util/attributes.hpp"
#include "event/command.hpp"
#include "fwd.hpp"
#include "hooks/breach.hpp"
#include "hooks/detail/screening.hpp"
#include "hooks/observer.hpp"
#include "hooks/pre_trade/duplicate.hpp"
#include "hooks/pre_trade/order_size_check.hpp"
#include "hooks/pre_trade/position.hpp"
#include "hooks/pre_trade/position_limit.hpp"
#include "hooks/pre_trade/price_collar.hpp"
#include "hooks/pre_trade/rate_limiter.hpp"
#include "hooks/pre_trade/working_ledger.hpp"
#include "hooks/system/circuit_breaker.hpp"
#include "hooks/system/global_kill_switch.hpp"
#include "hooks/system/pnl_drawdown_breaker.hpp"
#include "limits.hpp"
#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace exchange::risk {

/// @brief What one @ref risk_gate::mass_cancel got through to the sink.
struct mass_cancel_result {
	/// @brief CANCELs the sink accepted.
	std::size_t cancelled = 0;
	/// @brief Orders still working that were not asked to cancel, because the
	///        sink pushed back part-way. Zero means the whole ledger was sent.
	std::size_t remaining = 0;
};

/// @brief Whether the whole ledger was sent, rather than stopping on
///        back-pressure. @see risk_gate::mass_cancel
[[nodiscard]] constexpr bool
is_complete(const mass_cancel_result &result) noexcept {
	return result.remaining == 0;
}

/**
 * @brief Screens every command a strategy emits before the gateway sees it, and
 *        keeps the position and exposure that screening is measured against.
 *
 * @tparam Sink Where surviving commands go - @c execution::engine_partition, or
 *         another gate, or a test double. Needs @c bool @c submit_range(span).
 * @tparam Clock Where "now" comes from. @see core::chrono::nanosecond_clock
 * @tparam Observer Who is told when a rule refuses a command, when the gate
 *         trips the breaker, or when the sink pushes back. Defaults to
 *         @c no_observer, which hears nothing and costs nothing. @see
 *         gate_observer
 *
 * @par How it gets between the two without either knowing
 * A gate *is* a sink: it exposes the same @c submit_range a strategy host
 * already writes through, and forwards to the real one. So inserting it is a
 * change to one line of wiring and to nothing else -
 *
 * @code
 * execution::engine_partition<1024> partition{...};
 * risk::risk_gate gate(partition, BTCUSD, limits, positions, breaker);
 * auto host = strategy::compose(gate, BTCUSD, strategy::iceberg<>{10'000});
 * @endcode
 *
 * - and there is no dependency in either direction: @c strategy/ does not name
 * @c risk/, @c risk/ does not name @c strategy/, and the conformance is a
 * @c static_assert in the test tree, which is allowed to name both. Two gates
 * can be stacked (a per-strategy one inside a per-desk one) for the same
 * reason.
 *
 * @par Reading a gate versus being told by one
 * Everything an operator needs is a counter away - @c refused(), @c breaches(),
 * @c stalls() - and a console that polls those wants nothing else. What polling
 * cannot do is *act* on the individual refusal, because @c rejections() holds
 * only the last batch and a counter cannot say which order moved it. That is
 * what the @c Observer is for, and it is a template parameter rather than a
 * @c std::move_only_function member for the reason the sink is: the default has
 * to compile away completely, and a hook nobody defined must not cost a null
 * check on the refusal path. @see hooks/observer.hpp
 *
 * @par Where the rules are
 * One file each, under @c hooks/. This class is what calls them, in what
 * order, against what state - the rules themselves are free functions over
 * their inputs, so each has a name, a docstring and a test of its own without
 * any of them knowing about a gate. Nothing was made virtual to achieve that,
 * and nothing short-circuits: see below for why either would have cost more
 * than it bought. @see hooks/fwd.hpp for the map from the industry's list of
 * pre-trade checks onto this tree.
 *
 * @par The per-command check, and what "a few nanoseconds" actually means
 * The limit arithmetic does not short-circuit. Every rule is evaluated into a
 * register and its bit ORed into a mask, so ten rules cost ten compares and
 * *one* branch instead of ten branches - and a branch here is the expensive
 * kind, because "is this order too big" is unpredictable by construction.
 * Everything the rules read is hoisted out of the loop before it starts: the
 * clock is read once per batch, the breaker's state once, the rate window's
 * headroom once, and the position and working totals once, because this thread
 * is their only writer. What remains inside the loop is register arithmetic on
 * values already in L1.
 *
 * The honest exception is the working-order ledger. Inserting an order is a
 * hash probe, which is a loop, and it is deliberately reached only *after* the
 * arithmetic has already found nothing wrong - an order that failed a limit
 * never touches the table at all. @see working_ledger
 *
 * @par Screen, deliver, then commit
 * A risk rejection is not back-pressure and the two must not be confused. A
 * full queue means "try again"; a breached limit means "this will never work".
 * So
 * @c submit_range drops the refused commands, delivers the rest, and returns
 * @c true - returning @c false would make a host retry a command that is going
 * to be refused identically forever.
 *
 * That leaves the case where the *sink* refuses, and this is where the ordering
 * matters: the gate has by then already inserted the surviving orders into its
 * ledger, and the host is about to hand it the same batch again. So a refused
 * delivery is rolled back - the entries inserted for this batch are retired,
 * the rate window is not charged, the breaches are not counted and nothing is
 * reported - and the retry re-screens from exactly the state it started in.
 * @c submit_range is therefore idempotent under back-pressure, which is the
 * property that makes the retry loop in @c strategy_engine safe to put a gate
 * in front of.
 *
 * @par One gate, one listing
 * Like @c strategy_engine, and for a stronger reason: every limit here is
 * denominated in the listing's own ticks and lots, and adding tick-lots across
 * two instruments is arithmetic on incompatible units. @see risk_limits
 *
 * @par Threading
 * One gate, one thread - the producer thread of its sink, which is also the
 * thread that receives the partition's published engine::trades and outcomes
 * and feeds them b ack through @c on_trades / @c on_outcomes. Everything the
 * gate owns privately is therefore unsynchronised. The two objects it *shares*
 * -
 * @c position_book and @c circuit_breaker - are where the atomics are, and each
 * documents its own contract.
 */
template <class Sink,
		  core::chrono::nanosecond_clock Clock = core::chrono::steady_nanos,
		  hooks::risk_observer Observer        = hooks::no_observer>
class risk_gate {
public:
	/**
	 * @brief Build a gate over @p sink for @p symbol.
	 *
	 * @param sink Where surviving commands go. Must outlive the gate.
	 * @param symbol The listing this gate screens. Every command it is handed
	 *        must name it.
	 * @param limits The policy. Copied - it is read on every command and wants
	 *        to be in the gate's own cache line, not behind a pointer.
	 * @param positions Shared position state. Must outlive the gate and must
	 *        carry @p symbol.
	 * @param breaker Shared kill switch. Must outlive the gate.
	 * @param reference_price Opening mark, in ticks, for the fat-finger band
	 * and the exposure valuation. Zero means "not known yet" - the band stays
	 *        open and exposure values at zero until the first print. @see
	 *        on_trade
	 * @param clock Where "now" comes from.
	 * @param observer Who is told about refusals, trips and stalls. Copied, so
	 *        one that has to outlive the call carries a handle to whatever it
	 *        reports to rather than the state itself - the same shape @c Clock
	 *        uses. @see hooks/observer.hpp
	 */
	risk_gate(Sink &sink, symbol_id_t symbol, const risk_limits &limits,
			  hooks::pre_trade::position_book &positions,
			  hooks::system::circuit_breaker &breaker,
			  price_t reference_price = 0, Clock clock = {},
			  Observer observer = {})
		: sink_(&sink),
		  positions_(&positions),
		  breaker_(&breaker),
		  clock_(std::move(clock)),
		  observer_(observer),
		  limits_(limits),
		  rate_(limits.max_messages_per_window, limits.rate_window_log2_ns),
		  ledger_(limits.max_working_orders),
		  symbol_(symbol) {
		assert(positions.carries(symbol) &&
			   "the position book must be sized for this listing");
		set_reference_price(reference_price);
		// Reserved here so mass_cancel never allocates: it runs when something
		// has already gone wrong, and that is the worst moment to need the
		// allocator. One chunk is the most it ever holds. @see mass_cancel
		cancel_batch_.reserve(MASS_CANCEL_CHUNK);
	}

	// Pinned to the thread that drains it, like the partition it fronts and the
	// host that feeds it. Nothing here would break under a move; there is
	// simply nowhere for one to go, and allowing it would invite a gate to be
	// relocated out from under the host holding a reference to it.
	risk_gate(const risk_gate &)            = delete;
	risk_gate &operator=(const risk_gate &) = delete;
	risk_gate(risk_gate &&)                 = delete;
	risk_gate &operator=(risk_gate &&)      = delete;
	~risk_gate()                            = default;

	// --- the sink side: what a strategy host calls -------------------------

	/**
	 * @brief Screen @p batch, deliver what survives, and report the rest.
	 *
	 * @return @c false only when the sink refused delivery - genuine
	 *         back-pressure, with the gate's own state rolled back so the
	 * caller may hand the identical batch again. @c true when everything that
	 *         was going to reach the gateway did, *including* the case where
	 *         every command was refused on risk grounds and none was delivered.
	 *
	 * @note @c rejections() holds this call's refusals and is cleared at entry,
	 *       the same way @c engine_partition::drain clears its buffers - so it
	 *       always describes exactly one call and never accumulates silently.
	 */
	[[nodiscard]] bool
	submit_range(std::span<const engine::event::command> batch) {
		rejections_.clear();
		if (batch.empty()) return true;
		if (masks_.size() < batch.size()) {
			masks_.resize(batch.size());
			reserved_.resize(batch.size());
		}

		screen_state state = open_batch();

		hooks::breach_bits any = 0;
		std::size_t surviving  = 0;
		for (std::size_t i = 0; i < batch.size(); ++i) {
			const hooks::breach_bits mask =
				screen(batch[i], state, reserved_[i]);
			masks_[i] = mask;
			any |= mask;
			surviving += static_cast<std::size_t>(mask == 0);
		}

		if (!deliver(batch, any == 0, surviving)) {
			roll_back(batch);
			return false;
		}
		commit(batch, state);
		return true;
	}

	/// @brief Screen and deliver one command. @see submit_range
	[[nodiscard]] bool submit(const engine::event::command &cmd) {
		return submit_range(std::span<const engine::event::command>{&cmd, 1});
	}

	/**
	 * @brief Which rules @p cmd would break right now, changing nothing.
	 *
	 * The dry run: same arithmetic @c submit_range applies, against the same
	 * state, with neither the ledger nor the counters touched. Two uses - an
	 * operator asking why an order would be refused before sending it, and a
	 * benchmark timing the check in isolation.
	 *
	 * @warning Not a guarantee about the next @c submit_range. It cannot see
	 * the duplicate-id and ledger-capacity rules, which are only decided by the
	 * insert itself, and a fill on another thread can move the position between
	 * the two calls. An empty result means "no limit objects to this, as of
	 * now", which is the honest reading of any pre-engine::trade check.
	 */
	[[nodiscard]] hooks::breach_set
	inspect(const engine::event::command &cmd) const noexcept {
		const screen_state state = open_batch();
		switch (cmd.type) {
		case engine::event::command_type::PLACE:
			return hooks::breach_set::from_bits(
				place_limits(cmd.as_place(), state));
		case engine::event::command_type::ADD:
			return hooks::breach_set::from_bits(
				level_limits(cmd.as_level(), state));
		case engine::event::command_type::MODIFY:
			return hooks::breach_set::from_bits(
				amend_limits(cmd.as_modify(), project(cmd.as_modify()), state));
		case engine::event::command_type::CANCEL:
		case engine::event::command_type::REDUCE:
			return hooks::breach_set::from_bits(
				hooks::system::risk_reducing_breach(state.state));
		}
		return {};
	}

	/// @brief What the last @c submit_range refused, as outcomes a client can
	/// be
	///        told. Empty when nothing was refused.
	///
	/// @note Anonymous commands produce nothing here. An ADD or a REDUCE
	/// carries
	///       no order id, so there is no order for an outcome to name; the
	///       refusal is still counted in @c breaches(). @see orders::order::id
	[[nodiscard]] std::span<const engine::order_outcome>
	rejections() const noexcept EXCHANGE_LIFETIMEBOUND {
		return rejections_;
	}

	// --- the feedback side: what the event loop calls back -----------------

	/**
	 * @brief Apply one execution: move the position, retire the working
	 *        quantity, and re-mark the fat-finger band.
	 *
	 * Both of a engine::trade's ids are looked up, because both can be ours. A
	 * self-engine::trade then applies twice with opposite signs and nets to
	 * zero, which is the right answer and falls out rather than being
	 * special-cased.
	 *
	 * @note Quantity is moved *here* and not from the FILL outcome, because a
	 *       engine::trade is the only record that carries the execution price -
	 * an outcome names quantities and a status. Marking a fill at the order's
	 * own limit instead would overstate every aggressive buy.
	 */
	void on_trade(const engine::trade &execution) noexcept {
		apply_side(execution.aggressor, execution);
		apply_side(execution.resting, execution);
		set_reference_price(execution.price);
		check_loss();
	}

	/// @brief @c on_trade over a batch, in order.
	void on_trades(std::span<const engine::trade> executions) noexcept {
		for (const engine::trade &t : executions) on_trade(t);
	}

	/**
	 * @brief Apply one lifecycle record: retire what an order no longer has
	 *        working.
	 *
	 * Only the terminal-without-execution transitions do anything. A REJECTED
	 * order never entered the book, and a CANCELLED one has left it, so in both
	 * cases whatever the ledger still shows is no longer exposure. A FILL is
	 * already accounted for by the @c engine::trade that caused it - the
	 * partition publishes engine::trades before outcomes, so by the time a
	 * terminal FILL arrives the ledger entry is gone and this finds nothing to
	 * do.
	 *
	 * ACCEPTED and CANCEL_REJECTED change nothing: the first confirms exposure
	 * the gate counted at submission, and the second concerns an order that
	 * either filled or never existed.
	 */
	void on_outcome(const engine::order_outcome &record) noexcept {
		const bool terminal = record.type == engine::OutcomeType::REJECTED ||
							  record.type == engine::OutcomeType::CANCELLED;
		if (!terminal) return;
		if (const auto retired = ledger_.retire(record.id))
			positions_->remove_working(symbol_, retired->side, retired->taken);
	}

	/// @brief @c on_outcome over a batch, in order.
	void on_outcomes(std::span<const engine::order_outcome> records) noexcept {
		for (const engine::order_outcome &o : records) on_outcome(o);
	}

	/**
	 * @brief Re-mark the fat-finger band around @p price.
	 *
	 * Called for you on every print. Call it directly to seed the band before
	 * the first engine::trade, or to mark against a quote midpoint rather than
	 * the tape. The band's bounds are recomputed here - one division, once per
	 * price change - so that the check itself is a single unsigned compare.
	 */
	void set_reference_price(price_t price) noexcept {
		if (price == reference_price_) return;
		reference_price_ = price;
		band_ =
			hooks::pre_trade::price_band::around(price, limits_.price_band_bps);
	}

	// --- the emergency action ----------------------------------------------

	/// @brief Working orders one mass cancel walks, and sends, per batch. Sized
	///        so the scratch is a kilobyte of stack rather than a member
	///        proportional to @c risk_limits::max_working_orders.
	static constexpr std::size_t MASS_CANCEL_CHUNK = 64;

	/**
	 * @brief Emit a CANCEL for every order the gate believes is working.
	 *
	 * The other half of the kill switch. Tripping the breaker stops new orders;
	 * it does not pull the ones already resting, and a strategy that has just
	 * been cut off is exactly the one that will not pull them itself. This is
	 * what an operator calls so that a halt does not leave live quotes in a
	 * book nobody is managing.
	 *
	 * @return What got through, and what did not. @see is_complete
	 *
	 * @par Why this is not screened, when a strategy's cancel would be
	 * Because the thing @c HALTED distrusts is not present here. A cancel from
	 * a strategy is refused in that state for one reason - the strategy may be
	 * naming ids it invented - and every id below comes out of the gate's own
	 * ledger, so each one is an order this gate screened, admitted and is still
	 * counting as exposure. Screening them would refuse, in the emergency, the
	 * only commands certain to be about real orders.
	 * @see hooks::system::risk_reducing_breach
	 *
	 * @par Why it does not retire what it cancels
	 * An order is working until the venue says otherwise, and a CANCEL that has
	 * been sent is not a CANCEL that has been honoured - it can cross a fill in
	 * flight and come back @c CANCEL_REJECTED. Retiring here would drop the
	 * gate's exposure the moment the command was written, leaving it blind to
	 * orders that are still live. The ledger is retired by @c on_outcome, off
	 * the venue's own confirmation, exactly as it is for a strategy's cancel.
	 *
	 * @par Called, not fired
	 * Nothing here watches the breaker and does this automatically, for the
	 * reason @c heartbeat_monitor is polled rather than fired: a loop that
	 * never calls it never mass cancels, and that is better stated in a header
	 * than discovered during an incident. It is also not coupled to a state -
	 * an orderly shutdown wants the same walk with no breaker involved - so
	 * *when* to call it is the composition's decision.
	 *
	 * @par Back-pressure, and what a retry costs
	 * Delivery is chunked, so a full sink stops the walk part-way and
	 * @c remaining says how much of the ledger was never reached. Calling again
	 * restarts from the first slot and re-sends the CANCELs that already
	 * landed, because nothing was retired to mark them done. Those duplicates
	 * come back @c CANCEL_REJECTED, which @c on_outcome ignores. That is the
	 * deliberate trade: in an emergency a cancel sent twice is cheap and a
	 * cancel missed is not.
	 *
	 * @note Allocates nothing. The scratch is on the stack and @c cancel_batch_
	 *       was reserved at construction, so the path that runs when everything
	 *       else has gone wrong does not also need the allocator.
	 */
	[[nodiscard]] mass_cancel_result mass_cancel() {
		std::array<hooks::pre_trade::working_order, MASS_CANCEL_CHUNK> found{};
		hooks::pre_trade::ledger_cursor cursor{};
		std::size_t cancelled = 0;

		for (;;) {
			const hooks::pre_trade::ledger_scan scan =
				ledger_.snapshot(found, cursor);
			if (scan.written == 0) break;
			cursor = scan.next;

			cancel_batch_.clear();
			for (std::size_t i = 0; i < scan.written; ++i)
				cancel_batch_.push_back(
					engine::event::command::cancel(symbol_, found[i].id));

			if (!sink_->submit_range(cancel_batch_)) {
				// Counted as a stall for the same reason roll_back does:
				// saturation is not a refusal, and an operator reading
				// `stalls()` wants both.
				++stalls_;
				notify_stall(cancel_batch_.size());
				break;
			}
			cancelled += scan.written;
		}

		if (cancelled != 0) {
			// A cancel is a real message and crowds out new orders, which is
			// what screen_reducing already charges one for. It is never refused
			// on account of the window, here or there.
			// Narrowing is safe: `cancelled` never exceeds the ledger's size,
			// which is bounded by risk_limits::max_working_orders.
			rate_.charge(clock_.now(), static_cast<std::uint32_t>(cancelled));
			mass_cancelled_ += cancelled;
		}
		assert(cancelled <= ledger_.size() &&
			   "a mass cancel names only orders the ledger is holding");
		return {.cancelled = cancelled,
				.remaining =
					static_cast<std::size_t>(ledger_.size()) - cancelled};
	}

	// --- what an operator reads -------------------------------------------

	/// @brief The listing this gate screens.
	[[nodiscard]] symbol_id_t symbol() const noexcept { return symbol_; }

	/// @brief The policy in force.
	[[nodiscard]] const risk_limits &limits() const noexcept { return limits_; }

	/**
	 * @brief Realised plus unrealised profit at the current mark, in tick-lots.
	 *
	 * Negative is a loss. Zero while no mark is known, since an open position
	 * cannot be valued without one. @see position_snapshot::pnl
	 */
	[[nodiscard]] std::int64_t pnl() const noexcept {
		return positions_->snapshot(symbol_).pnl(reference_price_);
	}

	/// @brief The mark the band is measured around, in ticks. Zero until the
	///        first print.
	[[nodiscard]] price_t reference_price() const noexcept {
		return reference_price_;
	}

	/// @brief Lowest price the fat-finger band admits, in ticks.
	[[nodiscard]] price_t band_low() const noexcept { return band_.low; }

	/// @brief Highest price the fat-finger band admits, in ticks.
	[[nodiscard]] price_t band_high() const noexcept { return band_.high(); }

	/// @brief Orders the gate believes are still working.
	[[nodiscard]] std::uint32_t working_orders() const noexcept {
		return ledger_.size();
	}

	/// @brief CANCELs emitted by @c mass_cancel since construction, counting a
	///        re-send after back-pressure again. @see mass_cancel
	[[nodiscard]] std::uint64_t mass_cancelled() const noexcept {
		return mass_cancelled_;
	}

	/// @brief Commands delivered to the sink since construction.
	[[nodiscard]] std::uint64_t passed() const noexcept {
		return passed_count_;
	}

	/// @brief Commands refused on risk grounds since construction.
	[[nodiscard]] std::uint64_t refused() const noexcept {
		return refused_count_;
	}

	/// @brief Times the sink refused delivery and the batch was rolled back.
	///        Saturation, not error - @see strategy_engine::stalls.
	[[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

	/// @brief How many times @p rule has been the reason, counting every rule a
	///        refused command broke rather than only the one it was told.
	[[nodiscard]] std::uint64_t breaches(hooks::breach rule) const noexcept {
		const auto bits = static_cast<unsigned>(rule);
		if (bits == 0 || !std::has_single_bit(bits)) return 0;
		return breach_counts_[static_cast<std::size_t>(std::countr_zero(bits))];
	}

	/// @brief The rate window, for a caller that wants to know how close it is.
	[[nodiscard]] const hooks::pre_trade::rate_limiter &rate() const noexcept {
		return rate_;
	}

	/// @brief The gate's view of what is working, for reconciliation.
	[[nodiscard]] const hooks::pre_trade::working_ledger &
	ledger() const noexcept {
		return ledger_;
	}

	/// @brief The observer, for one that accumulates rather than forwards.
	[[nodiscard]] const Observer &observer() const noexcept {
		return observer_;
	}

private:
	// What a batch carries while it is being screened, the band a price is
	// measured against and the branchless rule-to-bit trick all live in
	// detail/screening.hpp. A template has no private section a caller cannot
	// read, so `detail` is what says these are not part of the interface.
	using screen_state = hooks::detail::screen_state;

	/**
	 * @brief Read everything the per-command rules need, once.
	 *
	 * @note One @c snapshot rather than @c net_lots plus two @c working_lots.
	 *       @c enable_hardening keeps @c assert live in optimised builds, so
	 * each of those three accessors carries its own bounds check - three checks
	 *       for three loads, where @c snapshot pays one for six. The three
	 * extra fields are free either way: they share the cache line the other
	 * three are already on.
	 *
	 *       @c BM_PositionSnapshot is consistently faster than
	 *       @c BM_PositionRead despite doing twice the loads, which is the
	 *       ordering this relies on. The *size* of the effect on the gate is
	 *       below what the benchmark machine can resolve - its between-run
	 * drift is larger - so this is a change made on the reasoning and on the
	 *       simpler code, not one with a measured win behind it. Do not quote a
	 *       number for it.
	 */
	[[nodiscard]] screen_state open_batch() const noexcept {
		const core::chrono::monotonic_time now = clock_.now();
		const hooks::pre_trade::position_snapshot holding =
			positions_->snapshot(symbol_);
		return {
			.now              = now,
			.state            = breaker_->state(),
			.headroom         = rate_.headroom(now),
			.base_net         = holding.net_lots,
			.base_working_bid = holding.working_bid_lots,
			.base_working_ask = holding.working_ask_lots,
		};
	}

	/**
	 * @brief Which rules @p cmd breaks, and - if none - the state it consumes.
	 *
	 * Screening and reserving are one step on purpose. Separating them would
	 * mean either a second pass over the batch or a provisional copy of the
	 * ledger, and both cost more than the rollback that pays for merging them.
	 * @see submit_range on why a rollback exists at all.
	 *
	 * @param reserved Out: lots this command added to an existing ledger entry,
	 *        which only a MODIFY ever does. Zero for everything else, including
	 *        a PLACE - that one is undone by retiring the id it claimed, and
	 *        there is nothing to remember. @see roll_back
	 */
	[[nodiscard]] hooks::breach_bits screen(const engine::event::command &cmd,
											screen_state &state,
											quantity_t &reserved) noexcept {
		assert(cmd.symbol == symbol_ &&
			   "a gate screens one listing; the writer stamps the symbol");
		reserved = 0;

		switch (cmd.type) {
		case engine::event::command_type::PLACE:
			return screen_place(cmd, state);
		case engine::event::command_type::ADD: return screen_add(cmd, state);
		case engine::event::command_type::MODIFY:
			return screen_modify(cmd, state, reserved);
		case engine::event::command_type::CANCEL:
		case engine::event::command_type::REDUCE: return screen_reducing(state);
		}
		return 0;
	}

	/**
	 * @brief Every rule that is pure arithmetic, evaluated without branching
	 * and without touching anything.
	 *
	 * Split out from @c screen_place because the two halves have genuinely
	 * different characters and the split is what makes the cheap one measurable
	 * on its own. This half reads registers and a couple of L1 lines and cannot
	 * fail; the other half probes a hash table and mutates the ledger. @c
	 * inspect exposes this one, and @c order_limits.bench.cpp times it.
	 *
	 * @note Five calls, no branch between them, one mask out. Each is a
	 *       @c constexpr function of its own inputs, so this compiles to the
	 *       same straight line it did when the arithmetic was written out here
	 *       - the split bought names and tests, not indirection.
	 *       @see hooks/fwd.hpp
	 */
	[[nodiscard]] hooks::breach_bits
	place_limits(const engine::orders::order &o,
				 const screen_state &state) const noexcept {
		hooks::breach_bits mask = 0;
		mask |= hooks::system::new_liquidity_breach(state.state);
		mask |= hooks::pre_trade::size_breaches(o.price, o.qty, limits_);
		mask |= hooks::pre_trade::collar_breach(band_, o.price);
		mask |= hooks::pre_trade::exposure_breaches(o,
													state,
													limits_,
													reference_price_);
		mask |= hooks::pre_trade::rate_breach(state);
		return mask;
	}

	/**
	 * @brief The full check: a client order that will rest, fill and be
	 *        reported. Reserves what it accepts.
	 */
	[[nodiscard]] hooks::breach_bits
	screen_place(const engine::event::command &cmd,
				 screen_state &state) noexcept {
		const engine::orders::order &o = cmd.as_place();
		const bool buying              = o.side == side_t::bid;
		const auto lots                = static_cast<volume_t>(o.qty);

		hooks::breach_bits mask = place_limits(o, state);

		// The one check that is not arithmetic, and so the one kept behind a
		// branch: an order that already failed above never probes the table. A
		// clean claim reserves the id, which is what roll_back undoes.
		// @see hooks/pre_trade/duplicate.hpp
		if (mask == 0) mask |= hooks::pre_trade::claim(ledger_, o);

		if (mask == 0) {
			(buying ? state.pending_bid : state.pending_ask) += lots;
			++state.charged;
		}
		return mask;
	}

	/**
	 * @brief Anonymous liquidity: the size and price checks, and nothing that
	 *        needs an id.
	 *
	 * @warning An ADD rests under the reserved id zero, which the book does not
	 *          index and never reports an outcome for. There is therefore no
	 *          event that could retire it, so its exposure is *not* tracked -
	 *          counting it would ratchet the gate closed over a session. The
	 *          fat-finger and size limits still apply, because those are about
	 *          the command rather than about what becomes of it. This is why
	 * ADD is a seeding command and not a trading one. @see
	 *          exchange::add_order
	 */
	[[nodiscard]] hooks::breach_bits
	screen_add(const engine::event::command &cmd,
			   screen_state &state) noexcept {
		const hooks::breach_bits mask = level_limits(cmd.as_level(), state);
		if (mask == 0) ++state.charged;
		return mask;
	}

	/// @brief The arithmetic half of @c screen_add, split for the same reason
	///        @c place_limits is. @see inspect
	///
	/// @note The same rules as @c place_limits, minus the position projection:
	///       an ADD carries no order id, so nothing could ever retire its
	///       exposure and counting it would ratchet the gate closed over a
	///       session. @see screen_add
	[[nodiscard]] hooks::breach_bits
	level_limits(const engine::event::level_change &lc,
				 const screen_state &state) const noexcept {
		hooks::breach_bits mask = 0;
		mask |= hooks::system::new_liquidity_breach(state.state);
		mask |= hooks::pre_trade::size_breaches(lc.price, lc.volume, limits_);
		mask |= hooks::pre_trade::collar_breach(band_, lc.price);
		mask |= hooks::pre_trade::rate_breach(state);
		return mask;
	}

	/**
	 * @brief What an amendment would add to the account, and to which side.
	 *
	 * The netting, and the reason a MODIFY is not screened as a fresh order: an
	 * amendment from 10 lots to 12 is two lots of new exposure, not twelve, and
	 * a gate that charged twelve would refuse a strategy that is barely moving.
	 *
	 * One struct rather than two accessors because it is one ledger probe. The
	 * probe is a hash lookup on the ingest path, and asking for the increase and
	 * the side separately made it three - @c screen_modify wants both and
	 * @c amend_limits wants both.
	 */
	struct amend_projection {
		/// @brief Lots on top of what is already working; zero when the
		///        amendment reduces, and zero for an order the ledger is not
		///        tracking - either one this gate never let through or one the
		///        venue has already finished with. Both reach the book, which
		///        answers UNKNOWN_ORDER; neither is exposure to project here.
		quantity_t added;
		/// @brief The ledger entry's side. An amendment carries none to give -
		///        it cannot move an order between the two books - and for an
		///        untracked order @c added is zero, so this does not matter.
		side_t side;
	};

	/**
	 * @brief Read @p change against the ledger. One probe.
	 *
	 * @warning The ledger holds *working* lots and an amendment carries the
	 *          *order* quantity, so for an order that has partially filled this
	 *          overstates the increase by what has already executed. That is
	 *          the conservative direction and it is the only one available: an
	 *          @c amendment carries no traded quantity, and the producer thread
	 *          this gate runs on cannot ask the book for one without depending
	 *          on the thing it exists to gate. @see working_ledger
	 */
	[[nodiscard]] amend_projection
	project(const engine::orders::amendment &change) const noexcept {
		const auto working = ledger_.find(change.id);
		if (!working) return {.added = 0, .side = side_t::bid};
		return {.added = change.quantity > working->lots
							 ? change.quantity - working->lots
							 : 0,
				.side  = working->side};
	}

	/// @brief The arithmetic half of @c screen_modify, split for the same
	///        reason @c place_limits is. @see inspect
	///
	/// @note The size and fat-finger rules read the amendment's *whole*
	///       quantity, and the exposure projection reads only what it adds.
	///       That is not an inconsistency: a per-order size cap is a statement
	///       about how big an order may be, which an amendment is asking to
	///       change, while a position limit is a statement about how much more
	///       the account may take on. The first is about the command, the
	///       second about the delta.
	[[nodiscard]] hooks::breach_bits
	amend_limits(const engine::orders::amendment &change,
				 amend_projection adds,
				 const screen_state &state) const noexcept {
		hooks::breach_bits mask = 0;
		// Only an increase asks the venue for anything new, so only an increase
		// is held to the new-liquidity state. An amendment that reduces or
		// merely reprices is risk-reducing and passes for the same reason a
		// cancel does. @see screen_reducing
		if (adds.added > 0)
			mask |= hooks::system::new_liquidity_breach(state.state);
		else mask |= hooks::system::risk_reducing_breach(state.state);
		mask |= hooks::pre_trade::size_breaches(change.price,
												change.quantity,
												limits_);
		mask |= hooks::pre_trade::collar_breach(band_, change.price);
		mask |= hooks::pre_trade::exposure_breaches(
			engine::orders::order{.id    = change.id,
								  .side  = adds.side,
								  .price = change.price,
								  .qty   = adds.added},
			state,
			limits_,
			reference_price_);
		mask |= hooks::pre_trade::rate_breach(state);
		return mask;
	}

	/**
	 * @brief An amendment: the same limits a new order faces, applied to what
	 *        the amendment actually adds.
	 *
	 * @par What is reserved, and what is not
	 * An increase moves the ledger entry onto the amended price and quantity
	 * and adds the difference to the batch's pending working lots, so a hundred
	 * amendments in one call are screened against each other exactly as a
	 * hundred orders would be. A *reduction* changes nothing here: the order is
	 * still working at its old quantity until the venue says otherwise, and the
	 * ledger is brought down by the fills and the terminal outcome that follow,
	 * as it always was. Counting a reduction at submission would leave the gate
	 * blind to exposure still resting in a book - the same argument
	 * @c mass_cancel makes for not retiring what it cancels.
	 */
	[[nodiscard]] hooks::breach_bits
	screen_modify(const engine::event::command &cmd, screen_state &state,
				  quantity_t &reserved) noexcept {
		const engine::orders::amendment &change = cmd.as_modify();
		const amend_projection adds             = project(change);

		const hooks::breach_bits mask = amend_limits(change, adds, state);
		if (mask != 0) return mask;

		++state.charged;
		if (adds.added == 0) return 0;

		(void)ledger_.amend(change.id, change.price, change.quantity);
		(adds.side == side_t::bid ? state.pending_bid : state.pending_ask) +=
			static_cast<volume_t>(adds.added);
		reserved = adds.added;
		return 0;
	}

	/**
	 * @brief A cancel or a reduction: risk-reducing, and therefore held to one
	 *        rule.
	 *
	 * @par Why these are not rate limited
	 * A throttle that blocks withdrawals is not a throttle, it is a trap: the
	 * moment a strategy most needs to pull its orders is the moment it has been
	 * sending the most, and refusing the cancel leaves live quotes in a book
	 * nobody is managing. So a cancel is *charged* against the window - it is a
	 * real message and it should crowd out new orders - but never refused on
	 * account of it.
	 *
	 * For the same reason only @c HALTED stops one, and @c HALTED is the state
	 * an operator selects by hand precisely when even the cancels are suspect.
	 * @see trading_state
	 */
	[[nodiscard]] hooks::breach_bits
	screen_reducing(screen_state &state) noexcept {
		const hooks::breach_bits mask =
			hooks::system::risk_reducing_breach(state.state);
		if (mask == 0) ++state.charged;
		return mask;
	}

	/// @brief Hand the survivors to the sink. Forwards @p batch untouched when
	///        nothing was refused, which is the case that has to be free.
	[[nodiscard]] bool deliver(std::span<const engine::event::command> batch,
							   bool all_clean, std::size_t surviving) {
		if (all_clean) return sink_->submit_range(batch);
		if (surviving == 0)
			return true; // nothing to deliver, nothing refused us

		survivors_.clear();
		survivors_.reserve(surviving);
		for (std::size_t i = 0; i < batch.size(); ++i)
			if (masks_[i] == 0) survivors_.push_back(batch[i]);
		return sink_->submit_range(survivors_);
	}

	/**
	 * @brief Undo the ledger inserts this batch made, leaving the gate exactly
	 *        as the batch found it.
	 *
	 * A command that screened clean is one this batch inserted - a pre-existing
	 * id would have come back as @c DUPLICATE_ORDER - so retiring every clean
	 * PLACE removes what this batch added and nothing else. The other
	 * provisional state needs no undoing: it lives in the @c screen_state,
	 * which is about to go out of scope.
	 *
	 * @par The one command that is undone rather than retired
	 * A MODIFY amends an entry that was already there, so retiring it would
	 * throw away an order the venue is still working - the gate would stop
	 * counting exposure it genuinely has, which is the failure a risk system
	 * must not have quietly. What it added is instead taken back off, which is
	 * why @c screen reports the amount: nothing else remembers what the entry
	 * held before.
	 *
	 * @note The amended *price* is not put back. It is not exposure - no rule
	 *       here reads a ledger entry's price - and restoring it would mean
	 *       remembering a second field per command to undo something nothing
	 *       tests. It is stale only until the next amendment or the order's
	 *       retirement, and only in @c mass_cancel's scratch, which cancels by
	 *       id.
	 */
	void roll_back(std::span<const engine::event::command> batch) noexcept {
		for (std::size_t i = 0; i < batch.size(); ++i) {
			if (masks_[i] != 0) continue;
			if (batch[i].type == engine::event::command_type::MODIFY) {
				if (reserved_[i] > 0)
					(void)ledger_.take(batch[i].as_modify().id, reserved_[i]);
				continue;
			}
			if (batch[i].type != engine::event::command_type::PLACE) continue;
			// What was retired is deliberately dropped: nothing was published
			// for it. The working quantity this batch reserved is still sitting
			// in the screen_state, and commit() is the call that would have
			// moved it into the position book.
			(void)ledger_.retire(batch[i].as_place().id);
		}
		++stalls_;
		notify_stall(batch.size());
	}

	/// @brief Publish everything the batch consumed, now that it has landed.
	void commit(std::span<const engine::event::command> batch,
				const screen_state &state) {
		// Guarded because these are the two lines that touch a cache line other
		// threads read: writing a counter its own value still takes the line
		// exclusive and invalidates every reader's copy.
		if (state.pending_bid != 0)
			positions_->add_working(symbol_, side_t::bid, state.pending_bid);
		if (state.pending_ask != 0)
			positions_->add_working(symbol_, side_t::ask, state.pending_ask);
		rate_.charge(state.now, state.charged);

		for (std::size_t i = 0; i < batch.size(); ++i) {
			if (masks_[i] == 0) {
				++passed_count_;
				continue;
			}
			++refused_count_;
			count_breaches(masks_[i]);
			report(batch[i], masks_[i]);
			notify_breach(batch[i], masks_[i]);
			// The return says this call is what tripped it, and it trips to
			// exactly this pair - so the hook reports the decision the breaker
			// made rather than re-reading a shared state an operator may have
			// changed in between. @see circuit_breaker::record_breach
			if (breaker_->record_breach(state.now))
				notify_halt(hooks::system::trading_state::CANCEL_ONLY,
							hooks::system::trip_cause::BREACH_RATE);
		}
	}

	/// @brief Tally every rule a refused command broke, not only the one it was
	///        told about - an operator diagnosing a strategy wants all of them.
	void count_breaches(hooks::breach_bits mask) noexcept {
		while (mask != 0) {
			const auto index = static_cast<std::size_t>(std::countr_zero(mask));
			if (index < hooks::detail::BREACH_BIT_COUNT)
				++breach_counts_[index];
			mask &= mask - 1; // clear the lowest set bit
		}
	}

	/// @brief Turn a refusal into the outcome a client is told, when the
	/// command
	///        names an order to tell them about.
	void report(const engine::event::command &cmd, hooks::breach_bits mask) {
		const engine::reject_reason reason =
			hooks::first_reason(hooks::breach_set::from_bits(mask));
		switch (cmd.type) {
		case engine::event::command_type::PLACE: {
			const engine::orders::order &o = cmd.as_place();
			rejections_.push_back(
				engine::order_outcome::rejected(o.id, reason, o.qty));
			break;
		}
		case engine::event::command_type::CANCEL:
			rejections_.push_back(
				engine::order_outcome::cancel_rejected(cmd.as_cancel(),
													   reason));
			break;
		case engine::event::command_type::MODIFY:
			rejections_.push_back(
				engine::order_outcome::modify_rejected(cmd.as_modify().id,
													   reason));
			break;
		case engine::event::command_type::ADD:
		case engine::event::command_type::REDUCE:
			break; // anonymous - no order for an outcome to name
		}
	}

	/**
	 * @brief Trip the breaker if profit has fallen through the floor.
	 *
	 * @par Why here and not in the per-command screen
	 * Profit moves only when something prints, and @c on_trade runs on *every*
	 * print - ours and everyone else's. So this catches both halves of a
	 * drawdown: a fill that realises a loss, and a market that moves against a
	 * position we are simply holding. Checking it per command would re-evaluate
	 * a number that cannot have changed, on the one path that cannot afford it.
	 *
	 * @note Marked at the price that just printed, because @c
	 * set_reference_price has already run. Marking at anything staler would let
	 * a gap through.
	 * @note Does not re-trip a breaker that is already open, so a strategy
	 *       bleeding through the floor produces one trip and one cause rather
	 *       than one per print.
	 */
	void check_loss() noexcept {
		// The profit goes in as a callable rather than a value: reading it
		// touches a line other threads write, and the hook rules the whole
		// check out without that read when no floor is configured or the
		// breaker is already open.
		// @see hooks/system/pnl_drawdown_breaker.hpp
		const bool tripped = hooks::system::trip_on_drawdown(
			*breaker_,
			limits_,
			[this]() noexcept { return pnl(); });
		if (tripped)
			notify_halt(hooks::system::trading_state::CANCEL_ONLY,
						hooks::system::trip_cause::LOSS_LIMIT);
	}

	/// @brief If @p id is one of ours, move its position and retire the lots
	///        this execution took.
	void apply_side(order_id_t id, const engine::trade &execution) noexcept {
		const auto taken = ledger_.take(id, execution.volume);
		if (!taken) return;
		positions_->apply_fill(symbol_,
							   taken->side,
							   execution.price,
							   taken->taken);
		positions_->remove_working(symbol_, taken->side, taken->taken);
	}

	// --- telling the observer ---------------------------------------------
	//
	// One function per hook, each an `if constexpr` on the concept, because
	// that is what makes the default free: with `no_observer` the branch is
	// discarded and these are empty inline functions, not calls that return.
	// The mask is widened into a `breach_set` here rather than at the call site
	// so the hot path keeps passing the bits it already has. @see
	// hooks/observer.hpp

	void notify_breach(const engine::event::command &cmd,
					   hooks::breach_bits mask) noexcept {
		if constexpr (hooks::breach_observer<Observer>)
			observer_.on_breach(cmd, hooks::breach_set::from_bits(mask));
	}

	void notify_halt(hooks::system::trading_state to,
					 hooks::system::trip_cause why) noexcept {
		if constexpr (hooks::halt_observer<Observer>)
			observer_.on_halt(to, why);
	}

	void notify_stall(std::size_t retained) noexcept {
		if constexpr (hooks::stall_observer<Observer>)
			observer_.on_stall(retained);
	}

	Sink *sink_;
	hooks::pre_trade::position_book *positions_;
	hooks::system::circuit_breaker *breaker_;
	EXCHANGE_NO_UNIQUE_ADDRESS Clock clock_;
	// Empty in the default configuration, and this is what keeps that free in
	// space as well as in time: a gate with `no_observer` is the same size as
	// one that never had the parameter.
	//
	// The attribute is a no-op rather than a constraint once an observer has
	// state to hold - it says "if this is empty, do not give it a unique
	// address", not "this must be empty". An observer carrying a handle to
	// whatever it reports to is the shape observer.hpp asks for, and it costs
	// its own size: `app::gate_logger` is one pointer, which takes this gate
	// from 360 bytes to 368 on a 64-bit build and lands in the slot the empty
	// member was not using. Paid on the refusal path, which was already
	// building an outcome record when it gets there.
	EXCHANGE_NO_UNIQUE_ADDRESS Observer observer_;

	risk_limits limits_;
	hooks::pre_trade::rate_limiter rate_;
	hooks::pre_trade::working_ledger ledger_;

	symbol_id_t symbol_;
	price_t reference_price_ = 0;
	hooks::pre_trade::price_band band_;

	// Reused across batches. They reach their high-water mark within the first
	// few calls and never allocate again, which is what the no-heap-on-ingest
	// invariant asks for - a fixed array would need the host's batch size as a
	// template parameter and would put it in the gate's type.
	std::vector<hooks::breach_bits> masks_;
	/// @brief Per command, lots it added to a ledger entry that already
	///        existed - a clean MODIFY and nothing else. Sized with
	///        @c masks_ and only ever read by @c roll_back.
	std::vector<quantity_t> reserved_;
	std::vector<engine::event::command> survivors_;
	std::vector<engine::order_outcome> rejections_;
	// Separate from survivors_ rather than sharing it: this one is reserved at
	// construction and must stay that way, and a mass cancel borrowing the
	// buffer a batch is delivered through would tie the two lifetimes together
	// for no saving worth having. @see mass_cancel
	std::vector<engine::event::command> cancel_batch_;

	std::array<std::uint64_t, hooks::detail::BREACH_BIT_COUNT> breach_counts_{};
	std::uint64_t passed_count_   = 0;
	std::uint64_t refused_count_  = 0;
	std::uint64_t stalls_         = 0;
	std::uint64_t mass_cancelled_ = 0;
};

} // namespace exchange::risk

#pragma once
// The pre-engine::trade check, sitting inline between a strategy host and the
// gateway.

#include "clock.hpp"
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
#include "event/command.hpp"
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
#include <vector>

namespace exchange::risk {

/**
 * @brief Screens every command a strategy emits before the gateway sees it, and
 *        keeps the position and exposure that screening is measured against.
 *
 * @tparam Sink Where surviving commands go - @c execution::engine_partition, or
 *         another gate, or a test double. Needs @c bool @c submit_range(span).
 * @tparam Clock Where "now" comes from. @see nanosecond_clock
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
 * @c std::function member for the reason the sink is: the default has to
 * compile away completely, and a hook nobody defined must not cost a null check
 * on the refusal path. @see hooks/observer.hpp
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
template <class Sink, nanosecond_clock Clock = steady_nanos,
		  hooks::risk_observer Observer = hooks::no_observer>
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
		  clock_(clock),
		  observer_(observer),
		  limits_(limits),
		  rate_(limits.max_messages_per_window, limits.rate_window_log2_ns),
		  ledger_(limits.max_working_orders),
		  symbol_(symbol) {
		assert(positions.carries(symbol) &&
			   "the position book must be sized for this listing");
		set_reference_price(reference_price);
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
		if (masks_.size() < batch.size()) masks_.resize(batch.size());

		screen_state state = open_batch();

		hooks::breach_bits any = 0;
		std::size_t surviving  = 0;
		for (std::size_t i = 0; i < batch.size(); ++i) {
			const hooks::breach_bits mask = screen(batch[i], state);
			masks_[i]                     = mask;
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
		case engine::event::command::Type::PLACE:
			return hooks::breach_set::from_bits(
				place_limits(cmd.as_place(), state));
		case engine::event::command::Type::ADD:
			return hooks::breach_set::from_bits(
				level_limits(cmd.as_level(), state));
		case engine::event::command::Type::CANCEL:
		case engine::event::command::Type::REDUCE:
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
	rejections() const noexcept {
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
		const monotonic_time now = clock_.now();
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
	 */
	[[nodiscard]] hooks::breach_bits screen(const engine::event::command &cmd,
											screen_state &state) noexcept {
		assert(cmd.symbol == symbol_ &&
			   "a gate screens one listing; the writer stamps the symbol");

		switch (cmd.type) {
		case engine::event::command::Type::PLACE:
			return screen_place(cmd, state);
		case engine::event::command::Type::ADD: return screen_add(cmd, state);
		case engine::event::command::Type::CANCEL:
		case engine::event::command::Type::REDUCE:
			return screen_reducing(state);
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
	 *          order_book::add_order
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
	 */
	void roll_back(std::span<const engine::event::command> batch) noexcept {
		for (std::size_t i = 0; i < batch.size(); ++i) {
			if (masks_[i] != 0) continue;
			if (batch[i].type != engine::event::command::Type::PLACE) continue;
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
			// TODO: loses the .count() once circuit_breaker takes a
			// monotonic_time too. @see risk::monotonic_clock
			if (breaker_->record_breach(static_cast<std::uint64_t>(
					state.now.time_since_epoch().count())))
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
		case engine::event::command::Type::PLACE: {
			const engine::orders::order &o = cmd.as_place();
			rejections_.push_back(
				engine::order_outcome::rejected(o.id, reason, o.qty));
			break;
		}
		case engine::event::command::Type::CANCEL:
			rejections_.push_back(
				engine::order_outcome::cancel_rejected(cmd.as_cancel(),
													   reason));
			break;
		case engine::event::command::Type::ADD:
		case engine::event::command::Type::REDUCE:
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
	[[no_unique_address]] Clock clock_;
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
	[[no_unique_address]] Observer observer_;

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
	std::vector<engine::event::command> survivors_;
	std::vector<engine::order_outcome> rejections_;

	std::array<std::uint64_t, hooks::detail::BREACH_BIT_COUNT> breach_counts_{};
	std::uint64_t passed_count_  = 0;
	std::uint64_t refused_count_ = 0;
	std::uint64_t stalls_        = 0;
};

} // namespace exchange::risk

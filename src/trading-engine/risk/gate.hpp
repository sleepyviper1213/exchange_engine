#pragma once
// The pre-trade check, sitting inline between a strategy host and the gateway.

#include "breach.hpp"
#include "circuit_breaker.hpp"
#include "clock.hpp"
#include "fwd.hpp"
#include "limits.hpp"
#include "position.hpp"
#include "rate_limiter.hpp"
#include "working_ledger.hpp"

#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace exchange::engine::risk {

/**
 * @brief Screens every command a strategy emits before the gateway sees it, and
 *        keeps the position and exposure that screening is measured against.
 *
 * @tparam Sink Where surviving commands go — @c execution::engine_partition, or
 *         another gate, or a test double. Needs @c bool @c submit_range(span).
 * @tparam Clock Where "now" comes from. @see nanosecond_clock
 *
 * @par How it gets between the two without either knowing
 * A gate *is* a sink: it exposes the same @c submit_range a strategy host
 * already writes through, and forwards to the real one. So inserting it is a
 * change to one line of wiring and to nothing else —
 *
 * @code
 * execution::engine_partition<1024> partition{...};
 * risk::risk_gate gate(partition, BTCUSD, limits, positions, breaker);
 * auto host = strategy::compose(gate, BTCUSD, strategy::iceberg<>{10'000});
 * @endcode
 *
 * — and there is no dependency in either direction: @c strategy/ does not name
 * @c risk/, @c risk/ does not name @c strategy/, and the conformance is a
 * @c static_assert in the test tree, which is allowed to name both. Two gates
 * can be stacked (a per-strategy one inside a per-desk one) for the same reason.
 *
 * @par The per-command check, and what "a few nanoseconds" actually means
 * The limit arithmetic does not short-circuit. Every rule is evaluated into a
 * register and its bit ORed into a mask, so ten rules cost ten compares and *one*
 * branch instead of ten branches — and a branch here is the expensive kind,
 * because "is this order too big" is unpredictable by construction. Everything
 * the rules read is hoisted out of the loop before it starts: the clock is read
 * once per batch, the breaker's state once, the rate window's headroom once, and
 * the position and working totals once, because this thread is their only
 * writer. What remains inside the loop is register arithmetic on values already
 * in L1.
 *
 * The honest exception is the working-order ledger. Inserting an order is a hash
 * probe, which is a loop, and it is deliberately reached only *after* the
 * arithmetic has already found nothing wrong — an order that failed a limit
 * never touches the table at all. @see working_ledger
 *
 * @par Screen, deliver, then commit
 * A risk rejection is not back-pressure and the two must not be confused. A full
 * queue means "try again"; a breached limit means "this will never work". So
 * @c submit_range drops the refused commands, delivers the rest, and returns
 * @c true — returning @c false would make a host retry a command that is going
 * to be refused identically forever.
 *
 * That leaves the case where the *sink* refuses, and this is where the ordering
 * matters: the gate has by then already inserted the surviving orders into its
 * ledger, and the host is about to hand it the same batch again. So a refused
 * delivery is rolled back — the entries inserted for this batch are retired, the
 * rate window is not charged, the breaches are not counted and nothing is
 * reported — and the retry re-screens from exactly the state it started in.
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
 * One gate, one thread — the producer thread of its sink, which is also the
 * thread that receives the partition's published trades and outcomes and feeds
 * them back through @c on_trades / @c on_outcomes. Everything the gate owns
 * privately is therefore unsynchronised. The two objects it *shares* —
 * @c position_book and @c circuit_breaker — are where the atomics are, and each
 * documents its own contract.
 */
template <class Sink, nanosecond_clock Clock = steady_nanos>
class risk_gate {
public:
	/**
	 * @brief Build a gate over @p sink for @p symbol.
	 *
	 * @param sink Where surviving commands go. Must outlive the gate.
	 * @param symbol The listing this gate screens. Every command it is handed
	 *        must name it.
	 * @param limits The policy. Copied — it is read on every command and wants
	 *        to be in the gate's own cache line, not behind a pointer.
	 * @param positions Shared position state. Must outlive the gate and must
	 *        carry @p symbol.
	 * @param breaker Shared kill switch. Must outlive the gate.
	 * @param reference_price Opening mark, in ticks, for the fat-finger band and
	 *        the exposure valuation. Zero means "not known yet" — the band stays
	 *        open and exposure values at zero until the first print. @see
	 *        on_trade
	 * @param clock Where "now" comes from.
	 */
	risk_gate(Sink &sink, symbol_id_t symbol, const risk_limits &limits,
			  position_book &positions, circuit_breaker &breaker,
			  price_t reference_price = 0, Clock clock = {})
		: sink_(&sink),
		  positions_(&positions),
		  breaker_(&breaker),
		  clock_(clock),
		  limits_(limits),
		  rate_(limits.max_messages_per_window, limits.rate_window_log2_ns),
		  ledger_(limits.max_working_orders),
		  symbol_(symbol) {
		assert(positions.carries(symbol) &&
			   "the position book must be sized for this listing");
		set_reference_price(reference_price);
	}

	// Pinned to the thread that drains it, like the partition it fronts and the
	// host that feeds it. Nothing here would break under a move; there is simply
	// nowhere for one to go, and allowing it would invite a gate to be relocated
	// out from under the host holding a reference to it.
	risk_gate(const risk_gate &)            = delete;
	risk_gate &operator=(const risk_gate &) = delete;
	risk_gate(risk_gate &&)                 = delete;
	risk_gate &operator=(risk_gate &&)      = delete;
	~risk_gate()                            = default;

	// --- the sink side: what a strategy host calls -------------------------

	/**
	 * @brief Screen @p batch, deliver what survives, and report the rest.
	 *
	 * @return @c false only when the sink refused delivery — genuine
	 *         back-pressure, with the gate's own state rolled back so the caller
	 *         may hand the identical batch again. @c true when everything that
	 *         was going to reach the gateway did, *including* the case where
	 *         every command was refused on risk grounds and none was delivered.
	 *
	 * @note @c rejections() holds this call's refusals and is cleared at entry,
	 *       the same way @c engine_partition::drain clears its buffers — so it
	 *       always describes exactly one call and never accumulates silently.
	 */
	[[nodiscard]] bool submit_range(std::span<const event::command> batch) {
		rejections_.clear();
		if (batch.empty()) return true;
		if (masks_.size() < batch.size()) masks_.resize(batch.size());

		screen_state state = open_batch();

		std::uint32_t any        = 0;
		std::size_t surviving    = 0;
		for (std::size_t i = 0; i < batch.size(); ++i) {
			const std::uint32_t mask = screen(batch[i], state);
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
	[[nodiscard]] bool submit(const event::command &cmd) {
		return submit_range(std::span<const event::command>{&cmd, 1});
	}

	/**
	 * @brief Which rules @p cmd would break right now, changing nothing.
	 *
	 * The dry run: same arithmetic @c submit_range applies, against the same
	 * state, with neither the ledger nor the counters touched. Two uses — an
	 * operator asking why an order would be refused before sending it, and a
	 * benchmark timing the check in isolation.
	 *
	 * @warning Not a guarantee about the next @c submit_range. It cannot see the
	 *          duplicate-id and ledger-capacity rules, which are only decided by
	 *          the insert itself, and a fill on another thread can move the
	 *          position between the two calls. An empty result means "no limit
	 *          objects to this, as of now", which is the honest reading of any
	 *          pre-trade check.
	 */
	[[nodiscard]] breach_set inspect(const event::command &cmd) const noexcept {
		const screen_state state = open_batch();
		switch (cmd.type) {
		case event::command::Type::PLACE:
			return breach_set::from_bits(place_limits(cmd.as_place(), state));
		case event::command::Type::ADD:
			return breach_set::from_bits(level_limits(cmd.as_level(), state));
		case event::command::Type::CANCEL:
		case event::command::Type::REDUCE:
			return breach_set::from_bits(
				bit_if(state.state == trading_state::HALTED, breach::HALTED));
		}
		return {};
	}

	/// @brief What the last @c submit_range refused, as outcomes a client can be
	///        told. Empty when nothing was refused.
	///
	/// @note Anonymous commands produce nothing here. An ADD or a REDUCE carries
	///       no order id, so there is no order for an outcome to name; the
	///       refusal is still counted in @c breaches(). @see orders::order::id
	[[nodiscard]] std::span<const order_outcome> rejections() const noexcept {
		return rejections_;
	}

	// --- the feedback side: what the event loop calls back -----------------

	/**
	 * @brief Apply one execution: move the position, retire the working
	 *        quantity, and re-mark the fat-finger band.
	 *
	 * Both of a trade's ids are looked up, because both can be ours. A
	 * self-trade then applies twice with opposite signs and nets to zero, which
	 * is the right answer and falls out rather than being special-cased.
	 *
	 * @note Quantity is moved *here* and not from the FILL outcome, because a
	 *       trade is the only record that carries the execution price — an
	 *       outcome names quantities and a status. Marking a fill at the order's
	 *       own limit instead would overstate every aggressive buy.
	 */
	void on_trade(const trade &execution) noexcept {
		apply_side(execution.aggressor, execution);
		apply_side(execution.resting, execution);
		set_reference_price(execution.price);
	}

	/// @brief @c on_trade over a batch, in order.
	void on_trades(std::span<const trade> executions) noexcept {
		for (const trade &t : executions) on_trade(t);
	}

	/**
	 * @brief Apply one lifecycle record: retire what an order no longer has
	 *        working.
	 *
	 * Only the terminal-without-execution transitions do anything. A REJECTED
	 * order never entered the book, and a CANCELLED one has left it, so in both
	 * cases whatever the ledger still shows is no longer exposure. A FILL is
	 * already accounted for by the @c trade that caused it — the partition
	 * publishes trades before outcomes, so by the time a terminal FILL arrives
	 * the ledger entry is gone and this finds nothing to do.
	 *
	 * ACCEPTED and CANCEL_REJECTED change nothing: the first confirms exposure
	 * the gate counted at submission, and the second concerns an order that
	 * either filled or never existed.
	 */
	void on_outcome(const order_outcome &record) noexcept {
		const bool terminal = record.type == OutcomeType::REJECTED ||
							  record.type == OutcomeType::CANCELLED;
		if (!terminal) return;
		if (const auto retired = ledger_.retire(record.id))
			positions_->remove_working(symbol_, retired->side, retired->taken);
	}

	/// @brief @c on_outcome over a batch, in order.
	void on_outcomes(std::span<const order_outcome> records) noexcept {
		for (const order_outcome &o : records) on_outcome(o);
	}

	/**
	 * @brief Re-mark the fat-finger band around @p price.
	 *
	 * Called for you on every print. Call it directly to seed the band before
	 * the first trade, or to mark against a quote midpoint rather than the tape.
	 * The band's bounds are recomputed here — one division, once per price
	 * change — so that the check itself is a single unsigned compare.
	 */
	void set_reference_price(price_t price) noexcept {
		if (price == reference_price_) return;
		reference_price_ = price;

		if (!limits_.has_price_band() || price == 0) {
			// An open band: every price is within `span` of zero.
			band_low_  = 0;
			band_span_ = std::numeric_limits<price_t>::max();
			return;
		}

		const auto mark = static_cast<std::int64_t>(price);
		const std::int64_t half =
			mark * limits_.price_band_bps / risk_limits::BPS_DENOMINATOR;
		// A price of zero ticks is never admissible, so the floor is one tick
		// rather than zero even for a band wider than the mark.
		const std::int64_t low  = mark - half > 1 ? mark - half : 1;
		const std::int64_t high = mark + half;
		band_low_  = static_cast<price_t>(low);
		band_span_ = static_cast<price_t>(high - low);
	}

	// --- what an operator reads -------------------------------------------

	/// @brief The listing this gate screens.
	[[nodiscard]] symbol_id_t symbol() const noexcept { return symbol_; }

	/// @brief The policy in force.
	[[nodiscard]] const risk_limits &limits() const noexcept { return limits_; }

	/// @brief The mark the band is measured around, in ticks. Zero until the
	///        first print.
	[[nodiscard]] price_t reference_price() const noexcept {
		return reference_price_;
	}

	/// @brief Lowest price the fat-finger band admits, in ticks.
	[[nodiscard]] price_t band_low() const noexcept { return band_low_; }

	/// @brief Highest price the fat-finger band admits, in ticks.
	[[nodiscard]] price_t band_high() const noexcept {
		return band_low_ + band_span_;
	}

	/// @brief Orders the gate believes are still working.
	[[nodiscard]] std::uint32_t working_orders() const noexcept {
		return ledger_.size();
	}

	/// @brief Commands delivered to the sink since construction.
	[[nodiscard]] std::uint64_t passed() const noexcept { return passed_count_; }

	/// @brief Commands refused on risk grounds since construction.
	[[nodiscard]] std::uint64_t refused() const noexcept {
		return refused_count_;
	}

	/// @brief Times the sink refused delivery and the batch was rolled back.
	///        Saturation, not error — @see strategy_engine::stalls.
	[[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

	/// @brief How many times @p rule has been the reason, counting every rule a
	///        refused command broke rather than only the one it was told.
	[[nodiscard]] std::uint64_t breaches(breach rule) const noexcept {
		const std::uint32_t bits = static_cast<std::uint32_t>(rule);
		if (bits == 0 || !std::has_single_bit(bits)) return 0;
		return breach_counts_[static_cast<std::size_t>(std::countr_zero(bits))];
	}

	/// @brief The rate window, for a caller that wants to know how close it is.
	[[nodiscard]] const rate_limiter &rate() const noexcept { return rate_; }

	/// @brief The gate's view of what is working, for reconciliation.
	[[nodiscard]] const working_ledger &ledger() const noexcept {
		return ledger_;
	}

private:
	/// @brief Everything hoisted out of the per-command loop, plus what the
	///        batch has provisionally used up so far.
	struct screen_state {
		std::uint64_t now_ns;
		trading_state state;
		std::uint32_t headroom;    ///< messages still allowed this window
		volume_t base_net;         ///< position at batch start
		volume_t base_working_bid; ///< working buys at batch start
		volume_t base_working_ask; ///< working sells at batch start
		volume_t pending_bid = 0;  ///< buys this batch has added
		volume_t pending_ask = 0;  ///< sells this batch has added
		std::uint32_t charged = 0; ///< messages this batch has used
	};

	[[nodiscard]] screen_state open_batch() const noexcept {
		const std::uint64_t now = clock_.now_ns();
		return {
			.now_ns   = now,
			.state    = breaker_->state(),
			.headroom = rate_.headroom(now),
			.base_net = positions_->net_lots(symbol_),
			.base_working_bid = positions_->working_lots(symbol_, side_t::bid),
			.base_working_ask = positions_->working_lots(symbol_, side_t::ask),
		};
	}

	/// @brief @p rule's bit when @p failed, zero otherwise — with no branch.
	///
	/// Negating a @c bool gives all-ones or all-zeros, and the AND then either
	/// keeps the bit or drops it. This is the whole trick, and it is why ten
	/// rules cost one branch between them.
	[[nodiscard]] static constexpr std::uint32_t bit_if(bool failed,
														breach rule) noexcept {
		return static_cast<std::uint32_t>(rule) &
			   -static_cast<std::uint32_t>(failed);
	}

	/// @brief Branchless absolute value. @see position_snapshot::abs_of
	[[nodiscard]] static constexpr volume_t abs_of(volume_t v) noexcept {
		return position_snapshot::abs_of(v);
	}

	/**
	 * @brief Which rules @p cmd breaks, and — if none — the state it consumes.
	 *
	 * Screening and reserving are one step on purpose. Separating them would
	 * mean either a second pass over the batch or a provisional copy of the
	 * ledger, and both cost more than the rollback that pays for merging them.
	 * @see submit_range on why a rollback exists at all.
	 */
	[[nodiscard]] std::uint32_t screen(const event::command &cmd,
									   screen_state &state) noexcept {
		assert(cmd.symbol == symbol_ &&
			   "a gate screens one listing; the writer stamps the symbol");

		switch (cmd.type) {
		case event::command::Type::PLACE:  return screen_place(cmd, state);
		case event::command::Type::ADD:    return screen_add(cmd, state);
		case event::command::Type::CANCEL:
		case event::command::Type::REDUCE: return screen_reducing(state);
		}
		return 0;
	}

	/**
	 * @brief Every rule that is pure arithmetic, evaluated without branching and
	 *        without touching anything.
	 *
	 * Split out from @c screen_place because the two halves have genuinely
	 * different characters and the split is what makes the cheap one measurable
	 * on its own. This half reads registers and a couple of L1 lines and cannot
	 * fail; the other half probes a hash table and mutates the ledger. @c inspect
	 * exposes this one, and @c order_limits.bench.cpp times it.
	 */
	[[nodiscard]] std::uint32_t
	place_limits(const orders::order &o,
				 const screen_state &state) const noexcept {
		const bool buying = o.side == side_t::bid;
		const auto lots   = static_cast<volume_t>(o.qty);

		// Widened before multiplying: a price near the top of price_t times a
		// quantity near the top of quantity_t is 9.0e18, which fits int64 —
		// just. Multiplying in 32 bits would not.
		const std::int64_t notional =
			static_cast<std::int64_t>(o.price) * static_cast<std::int64_t>(o.qty);

		// If this order and everything already working on each side filled.
		const volume_t bid_after =
			state.base_working_bid + state.pending_bid + (buying ? lots : 0);
		const volume_t ask_after =
			state.base_working_ask + state.pending_ask + (buying ? 0 : lots);
		const volume_t if_bids_fill = abs_of(state.base_net + bid_after);
		const volume_t if_asks_fill = abs_of(state.base_net - ask_after);
		const volume_t gross =
			if_bids_fill > if_asks_fill ? if_bids_fill : if_asks_fill;

		// This order alone, against the net position.
		const volume_t signed_lots = buying ? lots : -lots;
		const volume_t net_after   = abs_of(state.base_net + signed_lots);

		// One unsigned compare for a two-sided range: below the floor, the
		// subtraction wraps to something enormous and fails the same test.
		const auto from_floor = static_cast<price_t>(o.price - band_low_);

		std::uint32_t mask = 0;
		mask |= bit_if(state.state != trading_state::NORMAL, breach::HALTED);
		mask |= bit_if(o.qty <= 0, breach::NON_POSITIVE_QUANTITY);
		mask |= bit_if(o.qty > limits_.max_order_qty, breach::ORDER_QUANTITY);
		mask |= bit_if(notional > limits_.max_order_notional,
					   breach::ORDER_NOTIONAL);
		mask |= bit_if(from_floor > band_span_, breach::PRICE_BAND);
		mask |= bit_if(net_after > limits_.max_position_lots,
					   breach::POSITION_LIMIT);
		mask |= bit_if(gross * static_cast<volume_t>(reference_price_) >
						   limits_.max_exposure_notional,
					   breach::EXPOSURE_LIMIT);
		mask |= bit_if(state.charged >= state.headroom, breach::MESSAGE_RATE);
		return mask;
	}

	/**
	 * @brief The full check: a client order that will rest, fill and be
	 *        reported. Reserves what it accepts.
	 */
	[[nodiscard]] std::uint32_t screen_place(const event::command &cmd,
											 screen_state &state) noexcept {
		const orders::order &o = cmd.as_place();
		const bool buying      = o.side == side_t::bid;
		const auto lots        = static_cast<volume_t>(o.qty);

		std::uint32_t mask = place_limits(o, state);

		// The one check that is not arithmetic, and so the one kept behind a
		// branch: an order that already failed above never probes the table.
		if (mask == 0) {
			if (ledger_.full()) mask |= static_cast<std::uint32_t>(
				breach::WORKING_ORDERS);
			else if (!ledger_.insert(o.id, o.side, o.price, o.qty))
				mask |= static_cast<std::uint32_t>(breach::DUPLICATE_ORDER);
		}

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
	 *          event that could retire it, so its exposure is *not* tracked —
	 *          counting it would ratchet the gate closed over a session. The
	 *          fat-finger and size limits still apply, because those are about
	 *          the command rather than about what becomes of it. This is why ADD
	 *          is a seeding command and not a trading one. @see
	 *          order_book::add_order
	 */
	[[nodiscard]] std::uint32_t screen_add(const event::command &cmd,
										   screen_state &state) noexcept {
		const std::uint32_t mask = level_limits(cmd.as_level(), state);
		if (mask == 0) ++state.charged;
		return mask;
	}

	/// @brief The arithmetic half of @c screen_add, split for the same reason
	///        @c place_limits is. @see inspect
	[[nodiscard]] std::uint32_t
	level_limits(const event::level_change &lc,
				 const screen_state &state) const noexcept {
		const std::int64_t notional = static_cast<std::int64_t>(lc.price) *
									  static_cast<std::int64_t>(lc.volume);
		const auto from_floor = static_cast<price_t>(lc.price - band_low_);

		std::uint32_t mask = 0;
		mask |= bit_if(state.state != trading_state::NORMAL, breach::HALTED);
		mask |= bit_if(lc.volume <= 0, breach::NON_POSITIVE_QUANTITY);
		mask |= bit_if(lc.volume > limits_.max_order_qty,
					   breach::ORDER_QUANTITY);
		mask |= bit_if(notional > limits_.max_order_notional,
					   breach::ORDER_NOTIONAL);
		mask |= bit_if(from_floor > band_span_, breach::PRICE_BAND);
		mask |= bit_if(state.charged >= state.headroom, breach::MESSAGE_RATE);
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
	 * nobody is managing. So a cancel is *charged* against the window — it is a
	 * real message and it should crowd out new orders — but never refused on
	 * account of it.
	 *
	 * For the same reason only @c HALTED stops one, and @c HALTED is the state
	 * an operator selects by hand precisely when even the cancels are suspect.
	 * @see trading_state
	 */
	[[nodiscard]] std::uint32_t screen_reducing(screen_state &state) noexcept {
		const std::uint32_t mask =
			bit_if(state.state == trading_state::HALTED, breach::HALTED);
		if (mask == 0) ++state.charged;
		return mask;
	}

	/// @brief Hand the survivors to the sink. Forwards @p batch untouched when
	///        nothing was refused, which is the case that has to be free.
	[[nodiscard]] bool deliver(std::span<const event::command> batch,
							   bool all_clean, std::size_t surviving) {
		if (all_clean) return sink_->submit_range(batch);
		if (surviving == 0) return true; // nothing to deliver, nothing refused us

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
	 * A command that screened clean is one this batch inserted — a pre-existing
	 * id would have come back as @c DUPLICATE_ORDER — so retiring every clean
	 * PLACE removes what this batch added and nothing else. The other
	 * provisional state needs no undoing: it lives in the @c screen_state, which
	 * is about to go out of scope.
	 */
	void roll_back(std::span<const event::command> batch) noexcept {
		for (std::size_t i = 0; i < batch.size(); ++i) {
			if (masks_[i] != 0) continue;
			if (batch[i].type != event::command::Type::PLACE) continue;
			ledger_.retire(batch[i].as_place().id);
		}
		++stalls_;
	}

	/// @brief Publish everything the batch consumed, now that it has landed.
	void commit(std::span<const event::command> batch,
				const screen_state &state) {
		// Guarded because these are the two lines that touch a cache line other
		// threads read: writing a counter its own value still takes the line
		// exclusive and invalidates every reader's copy.
		if (state.pending_bid != 0)
			positions_->add_working(symbol_, side_t::bid, state.pending_bid);
		if (state.pending_ask != 0)
			positions_->add_working(symbol_, side_t::ask, state.pending_ask);
		rate_.charge(state.now_ns, state.charged);

		for (std::size_t i = 0; i < batch.size(); ++i) {
			if (masks_[i] == 0) {
				++passed_count_;
				continue;
			}
			++refused_count_;
			count_breaches(masks_[i]);
			report(batch[i], masks_[i]);
			breaker_->record_breach(state.now_ns);
		}
	}

	/// @brief Tally every rule a refused command broke, not only the one it was
	///        told about — an operator diagnosing a strategy wants all of them.
	void count_breaches(std::uint32_t mask) noexcept {
		while (mask != 0) {
			const auto index = static_cast<std::size_t>(std::countr_zero(mask));
			if (index < BREACH_BIT_COUNT) ++breach_counts_[index];
			mask &= mask - 1; // clear the lowest set bit
		}
	}

	/// @brief Turn a refusal into the outcome a client is told, when the command
	///        names an order to tell them about.
	void report(const event::command &cmd, std::uint32_t mask) {
		const reject_reason reason =
			first_reason(breach_set::from_bits(mask));
		switch (cmd.type) {
		case event::command::Type::PLACE: {
			const orders::order &o = cmd.as_place();
			rejections_.push_back(order_outcome::rejected(o.id, reason, o.qty));
			break;
		}
		case event::command::Type::CANCEL:
			rejections_.push_back(
				order_outcome::cancel_rejected(cmd.as_cancel(), reason));
			break;
		case event::command::Type::ADD:
		case event::command::Type::REDUCE:
			break; // anonymous — no order for an outcome to name
		}
	}

	/// @brief If @p id is one of ours, move its position and retire the lots
	///        this execution took.
	void apply_side(order_id_t id, const trade &execution) noexcept {
		const auto taken = ledger_.take(id, execution.volume);
		if (!taken) return;
		positions_->apply_fill(symbol_, taken->side, execution.price,
							   taken->taken);
		positions_->remove_working(symbol_, taken->side, taken->taken);
	}

	Sink *sink_;
	position_book *positions_;
	circuit_breaker *breaker_;
	[[no_unique_address]] Clock clock_;

	risk_limits limits_;
	rate_limiter rate_;
	working_ledger ledger_;

	symbol_id_t symbol_;
	price_t reference_price_ = 0;
	price_t band_low_        = 0;
	price_t band_span_       = std::numeric_limits<price_t>::max();

	// Reused across batches. They reach their high-water mark within the first
	// few calls and never allocate again, which is what the no-heap-on-ingest
	// invariant asks for — a fixed array would need the host's batch size as a
	// template parameter and would put it in the gate's type.
	std::vector<std::uint32_t> masks_;
	std::vector<event::command> survivors_;
	std::vector<order_outcome> rejections_;

	std::array<std::uint64_t, BREACH_BIT_COUNT> breach_counts_{};
	std::uint64_t passed_count_  = 0;
	std::uint64_t refused_count_ = 0;
	std::uint64_t stalls_        = 0;
};

} // namespace exchange::engine::risk

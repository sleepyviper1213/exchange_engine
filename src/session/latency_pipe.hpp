#pragma once
// The wire, on the live path.
//
// `backtest::wire` already models the gap between deciding to send a command
// and the venue having it, and it is clock-agnostic - so nothing about the
// *model* was missing here. What was missing is a driver: offline, market time
// moving forward is the wake-up, and a settle loop calls `deliver()` because it
// has just advanced the clock itself. A live run has no such moment. Wall-clock
// time passes whether or not a frame arrives, so a command that comes due
// during a quiet market comes due with nobody looking.
//
// This is the piece that closes that gap, and it is deliberately two things and
// not one: a sink the gate can write into, and a `deliver_due` an external
// timer can call. `serve.cpp` owns the timer, because owning timers is what it
// is for.

#include "core/chrono/clock.hpp"
#include "event/command.hpp"
#include "strategy/backtest/wire.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace exchange::session {

/**
 * @brief The order path, with or without a modelled wire in it.
 *
 * @tparam Sink What is downstream - @c crossing_fill_model on the live chain,
 *         and through it the partition.
 * @tparam Clock Where "now" comes from. Wall-clock in a deployment, a clock a
 *         test moves by hand in the suite. @see core::chrono::nanosecond_clock
 *
 * @par Why a wrapper rather than a wire in the chain
 * Because a live session must be able to have *no* modelled latency, and mean
 * it. A wire holding a zero delay still holds the command until somebody calls
 * @c deliver, which on this path is a different turn of the io_context - so
 * "zero latency" would not be the chain as it is today, it would be the chain
 * plus a scheduling hop. That is the one configuration whose behaviour has to
 * be unchanged, since it is what every existing test and every run so far
 * measured.
 *
 * So the model is read once, at construction, and decides which of two shapes
 * this is for the rest of its life: a pass-through that calls the sink
 * synchronously, exactly as the gate used to, or a wire. One branch on an
 * immutable flag, on a path that has already been through a hash probe in the
 * gate. That is the same trade @c live_session_options::simulate_fills already
 * documents for the fill model - splice it always, decide at run time - and the
 * reason is the same: the alternative is a second instantiation of the whole
 * session chosen at run time, which is a large compile-time cost to remove one
 * predictable branch.
 *
 * @par What it does not do
 * Reorder, drop, or duplicate. Those belong to @c wire, which does none of them
 * either, and the argument for that is in its own header: a wire that lost
 * messages layers a second failure mode onto a harness whose job is to isolate
 * one. A full schedule refuses at submission so the gate can roll back and the
 * run can count it.
 *
 * @note Producer thread only, like everything else it is composed with. It
 *       holds no atomics and is not safe to call from the consumer.
 */
template <class Sink, core::chrono::nanosecond_clock Clock>
class latency_pipe {
public:
	using command   = engine::event::command;
	using wire_type = strategy::backtest::wire<Sink, Clock>;
	using model     = strategy::backtest::latency_model;

	/**
	 * @brief Put @p model between @p sink and whatever writes into this.
	 *
	 * @param sink The far side. Must outlive the pipe.
	 * @param clock Read for the due times, and only when a wire exists.
	 * @param latency The delays. All-zero - the default - builds the
	 *        pass-through and no wire at all, which is today's chain.
	 */
	latency_pipe(Sink &sink, Clock clock, model latency = {}) : sink_(&sink) {
		if (latency.order_entry_ns != 0 || latency.jitter_ns != 0)
			wire_.emplace(sink, clock, latency);
	}

	// --- the sink side, which is all the gate knows about -------------------

	/**
	 * @brief Accept @p batch, either onto the wire or straight through.
	 * @return @c false if it was refused, holding none of it, so the gate's
	 *         rollback stays correct either way. A pass-through refuses because
	 *         the partition's queue is full; a wire refuses because too much of
	 *         ours is already in flight.
	 *
	 * @par Only our own orders go on the wire
	 * The gate carries two kinds of traffic on this path, and only one of them
	 * crossed a network. PLACE and CANCEL are ours: we decided to send them,
	 * and the time they spend in flight is the thing being modelled. ADD and
	 * REDUCE are the venue's own published depth being mirrored into our book
	 * by
	 * @c depth_feed_bridge - liquidity that already exists at the venue, which
	 * this process is copying rather than sending. Delaying that would model a
	 * *market_data* delay, which is deliberately not modelled (@see
	 * latency_model), and it would do it incoherently: the strategy reads the
	 * replica, which is updated the instant a frame lands, so the engine's book
	 * would lag a market its own strategy could already see.
	 *
	 * The offline harness draws the same line by routing rather than by
	 * inspection - @c backtest::session::submit_direct puts feed commands into
	 * the partition without passing the gate or the wire at all. It cannot be
	 * routed that way here, because a live session deliberately screens seeded
	 * depth through the gate too, and a gate has one sink. So the rule is
	 * applied where the traffic converges instead, which has the advantage of
	 * being stated rather than implied by which function was called.
	 *
	 * @note Classified per *batch*, never per command: a batch is submitted
	 *       all-or-nothing, and splitting one across two paths would deliver
	 *       half of it while telling the gate to roll back all of it. A batch
	 *       that mixes the two therefore goes on the wire whole, which is the
	 *       conservative direction. In this composition they never do mix - the
	 *       bridge's commands and the quoter's arrive as separate batches - so
	 *       the rule is exact here and safe if that ever changes.
	 */
	[[nodiscard]] bool submit_range(std::span<const command> batch) {
		if (wire_ && !is_mirrored_depth(batch))
			return wire_->submit_range(batch);
		return sink_->submit_range(batch);
	}

	// --- the driver side ---------------------------------------------------

	/**
	 * @brief Hand downstream everything whose flight time has elapsed.
	 *
	 * @return Commands delivered. Always zero without a wire, where there is
	 *         nothing in flight to deliver - a pass-through has already
	 *         delivered everything it accepted.
	 *
	 * Safe to call at any time and as often as anyone likes: it releases what
	 * the clock says is due and nothing else, so an early call is a no-op
	 * rather than a delivery ahead of schedule. That is what lets both a timer
	 * and the frame path call it without coordinating.
	 */
	std::size_t deliver_due() {
		if (!wire_) return 0;
		return wire_->deliver();
	}

	/**
	 * @brief When the earliest command in flight comes due, if any.
	 *
	 * What a timer arms itself from. Empty means there is nothing waiting on
	 * *time* - which is not the same as nothing waiting: a delivery the sink
	 * had no room for is already due and waiting on room. @see is_stalled
	 */
	[[nodiscard]] std::optional<std::uint64_t> next_due_ns() const noexcept {
		if (!wire_) return std::nullopt;
		return wire_->next_due_ns();
	}

	/// @brief Is a delivery held up by a downstream that had no room, rather
	///        than by time? Such a delivery is retried on the next
	///        @c deliver_due and never dropped.
	[[nodiscard]] bool is_stalled() const noexcept {
		return wire_ && wire_->is_stalled();
	}

	// --- what a report reads ------------------------------------------------

	/// @brief Is there a modelled wire at all? False is the pass-through, and
	///        then every counter below is zero for the run's whole life.
	[[nodiscard]] bool is_modelled() const noexcept {
		return wire_.has_value();
	}

	/// @brief Commands of ours the engine has not been told about yet.
	[[nodiscard]] std::size_t in_flight() const noexcept {
		return wire_ ? wire_->in_flight() : 0;
	}

	/// @brief Commands the wire has handed downstream since construction.
	[[nodiscard]] std::uint64_t delivered() const noexcept {
		return wire_ ? wire_->delivered() : 0;
	}

	/// @brief Batches refused because too much of ours was already in flight.
	[[nodiscard]] std::uint64_t refusals() const noexcept {
		return wire_ ? wire_->refusals() : 0;
	}

	/// @brief Deliveries downstream had no room for, and that were retried.
	[[nodiscard]] std::uint64_t stalls() const noexcept {
		return wire_ ? wire_->stalls() : 0;
	}

private:
	/// @brief Is every command in @p batch the venue's depth being mirrored,
	///        rather than an order of ours? An empty batch counts as depth: it
	///        contains nothing of ours to delay. @see submit_range
	[[nodiscard]] static bool
	is_mirrored_depth(std::span<const command> batch) noexcept {
		return std::ranges::all_of(batch, [](const command &cmd) noexcept {
			return cmd.type == command::Type::ADD ||
				   cmd.type == command::Type::REDUCE;
		});
	}

	Sink *sink_;
	/// Absent is the pass-through, and absent is the default. @see the class
	/// note on why zero latency must not mean "a wire with a zero delay".
	std::optional<wire_type> wire_;
};

} // namespace exchange::session

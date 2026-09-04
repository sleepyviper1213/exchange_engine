#pragma once
// The gap between deciding to send an order and the venue having it.
//
// Sits between the risk gate and the engine, which is exactly where the network
// sits in a deployment: everything above it has decided to send a command, and
// nothing below it can tell that the command was not applied the instant it was
// written.

#include "core/chrono/clock.hpp"
#include "event/command.hpp"
#include "fwd.hpp"
#include "scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace exchange::strategy::backtest {

/// @brief How long a command of ours spends in flight, in market time.
struct latency_model {
	/**
	 * @brief Nanoseconds between a strategy writing a command and the engine
	 *        applying it.
	 *
	 * Zero - the default - reproduces the harness as it behaved before any of
	 * this existed: a command comes due at the moment it was written, so it is
	 * released in the same settle round and nothing downstream can observe that
	 * it went through a queue. That equivalence is deliberate and is what makes
	 * the number the only thing a run has to be re-read against.
	 *
	 * @par Why one delay and not two
	 * The honest model has two: our order takes time to reach the venue, and
	 * the venue's market data took time to reach us, so the strategy is acting
	 * on a view that was already old. Only the order half is modelled here, and
	 * the reason it is a fair approximation is that the two enter the result
	 * through their sum - an order placed on information from time @c T lands
	 * at @c T plus both delays either way. Set this to the whole round trip and
	 * the arithmetic is right.
	 *
	 * What the approximation does lose is the strategy's *blindness* during the
	 * data delay: here it sees every frame, just late in its consequences,
	 * where a real one would never have seen the frames that arrived while its
	 * view was in flight. Modelling that means delaying the market and event
	 * hooks too, which is a change to the settle loop rather than to this file.
	 */
	std::uint64_t order_entry_ns = 0;

	/**
	 * @brief Uniform extra delay on top of @c order_entry_ns, per message.
	 *
	 * Drawn once per submitted batch, not once per command: a batch is one
	 * message on one wire, and its contents cannot overtake each other. What
	 * *can* reorder is two batches whose draws differ, which is the whole
	 * reason the schedule is a heap rather than a queue. @see delay_queue
	 *
	 * Zero by default, because a jittered run answers a different question: it
	 * is one sample from a distribution rather than a number, and comparing two
	 * strategy revisions across two samples measures the draw as much as the
	 * change. Turn it on to ask how sensitive a result is to timing, and
	 * compare like seeds with like.
	 */
	std::uint64_t jitter_ns = 0;

	/**
	 * @brief Seed for the jitter draw. Part of the run's identity.
	 *
	 * Fixed rather than seeded from a clock or a device, so a run remains a
	 * pure function of its input - the same capture and the same seed produce
	 * the same report, byte for byte, on any machine. A different seed is a
	 * different sample and not a bug. Irrelevant while @c jitter_ns is zero.
	 */
	std::uint64_t seed = 0x243F'6A88'85A3'08D3ULL;

	/// @brief The most commands of ours that may be in flight at once. A batch
	///        that does not fit is refused whole, which the gate sees as
	///        back-pressure and rolls back. @see wire::submit_range
	std::size_t max_in_flight = 4096;
};

/**
 * @brief Holds our commands for as long as the wire would have, then hands them
 *        to the engine.
 *
 * @tparam Sink What is on the far side of the wire - @c crossing_fill_model in
 *         the harness, and through it the partition.
 * @tparam Clock Where "now" comes from. @see core::chrono::clock_view
 *
 * @par Where it sits, and why there
 * The chain is trader, gate, wire, fill model, partition. Above the fill model
 * rather than below it, which matters for more than tidiness: the fill model
 * learns which orders are ours by watching the commands that pass through it,
 * so putting the delay above means its working set is "orders the engine has
 * been told about" rather than "orders we have written". Those differ by
 * exactly the flight time, and the first is the one the model needs - an order
 * still on the wire is not in any book and cannot be filled against.
 *
 * @par Why it reads the clock instead of being told the time
 * Its own interface gives it no choice for scheduling: it is a
 * @c strategy::command_sink, and @c submit_range takes commands and nothing
 * else. Given that, taking the time for *delivery* as an argument would allow a
 * caller to release at a time the schedule was never written against, which is
 * a bug with no symptom until a fill appears in the wrong frame. One clock,
 * read from one place, cannot be driven inconsistently.
 *
 * @par What it deliberately does not do
 * Nothing is ever dropped or reordered. A wire that lost messages would be a
 * second failure mode layered onto a harness whose job is to isolate one, and a
 * strategy's own resend logic is not what a fill model is measuring. A full
 * queue refuses at submission, where the gate can roll back and the run can
 * count it, rather than accepting a command it will not deliver.
 *
 * @warning Commands still in flight when the capture ends are never delivered -
 *          there is no market left to apply them against. That is the tail of
 *          the wire rather than a fault, and @c in_flight is what reports it.
 */
template <class Sink, core::chrono::nanosecond_clock Clock>
class wire {
public:
	using command = engine::event::command;

	/**
	 * @brief Build a wire from @p clock into @p sink.
	 * @param sink The far side. Must outlive the wire.
	 * @param clock Market time, by value - the same shape the gate holds one.
	 * @param model The delays to apply. @see latency_model
	 */
	wire(Sink &sink, Clock clock, latency_model model = {})
		: sink_(&sink),
		  clock_(std::move(clock)),
		  model_(model),
		  state_(model.seed),
		  schedule_(model.max_in_flight) {
		pending_.reserve(model.max_in_flight);
	}

	// --- the sink side ------------------------------------------------------

	/**
	 * @brief Accept @p batch onto the wire, due one flight time from now.
	 *
	 * @return @c false if the wire is full, in which case it holds none of the
	 *         batch. All-or-nothing, so the gate's rollback stays correct.
	 *
	 * @note Every command in one batch is given the same due time and a
	 *       distinct sequence number, so they arrive together and in the order
	 *       written. @see delay_queue::schedule_range
	 *
	 * @note Room is checked before the flight time is drawn, so a batch the
	 *       gate rolls back leaves the jitter sequence alone. Otherwise a
	 *       refusal would consume a draw, and the delays a run applied would
	 *       depend on how full the wire happened to get - reproducible, but for
	 *       a reason nobody could reconstruct from the capture and the seed.
	 */
	[[nodiscard]] bool submit_range(std::span<const command> batch) {
		if (batch.empty()) return true;
		if (!schedule_.has_room(batch.size())) {
			++refusals_;
			return false;
		}
		const std::uint64_t due = clock_.now_ns() + flight_time();
		return schedule_.schedule_range(due, batch);
	}

	/// @brief @c submit_range for one command.
	[[nodiscard]] bool submit(const command &cmd) {
		return submit_range(std::span<const command>{&cmd, 1});
	}

	// --- the harness side --------------------------------------------------

	/**
	 * @brief Hand the engine everything due by now.
	 *
	 * @return How many commands reached the sink. Zero either because nothing
	 *         was due or because the sink refused what was.
	 *
	 * @par Back-pressure is retried, never dropped
	 * A sink that refuses leaves the released commands staged here, in front of
	 * whatever comes due next, and the following call tries the whole lot
	 * again. That is the same choice @c event_channel makes on the return path
	 * and for the same reason: a full ring is a statement about room, not about
	 * the commands, and evicting them would silently change what the run did.
	 */
	std::size_t deliver() {
		(void)schedule_.release(clock_.now_ns(), pending_);
		if (pending_.empty()) return 0;
		if (!sink_->submit_range(pending_)) {
			++stalls_;
			return 0;
		}
		const std::size_t delivered = pending_.size();
		pending_.clear();
		delivered_ += delivered;
		return delivered;
	}

	// --- what a report reads -----------------------------------------------

	/// @brief Commands of ours the engine has not been told about yet, staged
	///        ones included.
	[[nodiscard]] std::size_t in_flight() const noexcept {
		return schedule_.pending() + pending_.size();
	}

	/// @brief Whether a delivery is held up by a sink that had no room.
	[[nodiscard]] bool is_stalled() const noexcept { return !pending_.empty(); }

	/// @brief When the next command comes due, or nothing if none is scheduled.
	/// @note Says nothing about a staged delivery, which is already due and
	///       waiting on room rather than on time. @see is_stalled
	[[nodiscard]] std::optional<std::uint64_t> next_due_ns() const noexcept {
		return schedule_.next_due_ns();
	}

	/// @brief Commands handed to the engine since construction.
	[[nodiscard]] std::uint64_t delivered() const noexcept {
		return delivered_;
	}

	/// @brief Batches refused because the wire was full. @see submit_range
	[[nodiscard]] std::uint64_t refusals() const noexcept { return refusals_; }

	/// @brief Deliveries the sink had no room for and that were retried.
	[[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

	/// @brief The delays this run was measured under.
	[[nodiscard]] const latency_model &model() const noexcept { return model_; }

private:
	/// @brief How long the next message spends in flight.
	[[nodiscard]] std::uint64_t flight_time() noexcept {
		if (model_.jitter_ns == 0) return model_.order_entry_ns;
		return model_.order_entry_ns + (next_draw() % (model_.jitter_ns + 1));
	}

	/**
	 * @brief SplitMix64: the jitter source.
	 *
	 * Chosen because it is four lines, has no state beyond a counter, and gives
	 * the same sequence on every platform and every standard library - which
	 * @c std::uniform_int_distribution explicitly does not, its mapping being
	 * unspecified. A run's reproducibility must not depend on which library it
	 * was built against. @see latency_model::seed
	 */
	[[nodiscard]] std::uint64_t next_draw() noexcept {
		state_ += 0x9E37'79B9'7F4A'7C15ULL;
		std::uint64_t z = state_;
		z               = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
		z               = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
		return z ^ (z >> 31);
	}

	Sink *sink_;
	Clock clock_;
	latency_model model_;
	/// SplitMix64's whole state. Seeded from @c latency_model::seed.
	std::uint64_t state_;

	delay_queue<command> schedule_;
	/// Released and not yet accepted by the sink. @see deliver
	std::vector<command> pending_;

	std::uint64_t delivered_ = 0;
	std::uint64_t refusals_  = 0;
	std::uint64_t stalls_    = 0;
};

} // namespace exchange::strategy::backtest

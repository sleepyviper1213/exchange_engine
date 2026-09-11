#pragma once
// Where an order stops being an idea and becomes something to send.
//
// `latency_pipe` sits between the gate and the engine and models a network.
// This sits between the gate and that pipe and writes to a real one - or
// rather, writes to an outbox a coroutine empties, because `submit_range` is a
// synchronous call on the producer thread and a socket is not.
//
// --- what it does not do ----------------------------------------------------
//
// Decide. The gate has already screened every command that reaches here, and
// `venue_gateway` answers the three questions that are left - budget, cap,
// breaker - and refuses in its own vocabulary. This is the wiring between them
// and the outbox, and it holds no policy of its own.
//
// It also does not *send*. Nothing here awaits anything, which is what lets the
// whole order path stay on the synchronous side of the session: the strategy
// writes, the gate screens, a request is built and queued, and the frame is
// over. What happens on the socket happens on some later turn of the
// io_context, and cannot make a frame wait for the venue.
//
// --- why a refusal here is not a refusal upstream ---------------------------
//
// The tempting design is to fail `submit_range` when the gateway declines, so
// the gate rolls back and the engine never records an order the venue does not
// have. It is wrong, and the reason is the retry loop above it:
// `live_session::quote` retries a refused batch until it is accepted. A
// rate-limit refusal would clear when the window rolls, so that spins for up to
// a second; an `order_cap_reached` refusal never clears, and the loop is then a
// hang rather than back-pressure.
//
// So the batch goes downstream whole, always, and a refusal is counted and
// reported instead. The consequence is stated rather than hidden: the engine's
// book is this process's record of what it *decided*, and the venue's is a
// record of what it *accepted*. Those can differ, they are meant to be
// comparable rather than identical, and `session::reconcile` is the function
// that compares them.

#include "event/command.hpp"
#include "session/venue_gateway.hpp"
#include "symbol/symbol_spec.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace exchange::session {

/// @brief What a router did with the orders that passed through it.
struct router_stats {
	std::uint64_t offered   = 0; ///< PLACE / CANCEL commands seen
	std::uint64_t queued    = 0; ///< requests built and put in the outbox
	std::uint64_t refused   = 0; ///< requests the gateway declined to build
	std::uint64_t discarded = 0; ///< orders dropped for want of outbox room

	/// @brief Requests waiting to be sent, right now. A gauge, not a total -
	///        a number that stays high is the socket failing to keep up with
	///        the strategy, which no cumulative counter can say.
	std::size_t queued_now = 0;
};

/**
 * @brief Copies this process's own orders out to a venue, and passes everything
 *        through to the engine.
 *
 * @tparam Sink What is downstream - @c latency_pipe on the live chain, and
 *         through it the fill model and the partition.
 *
 * @par Only our own orders are offered
 * PLACE and CANCEL are ours; ADD and REDUCE are the venue's published depth
 * being mirrored into our book by @c depth_feed_bridge. Sending mirrored depth
 * back to the venue it came from would be absurd, and the line between the two
 * is drawn here exactly as @c latency_pipe draws it - by inspecting the tag,
 * where the traffic converges, rather than by trusting which function called.
 *
 * @par Classified per command, not per batch
 * The opposite of @c latency_pipe, and deliberately: it must classify per batch
 * because it either delays the whole batch or none of it, and splitting one
 * would deliver half while reporting a refusal for all. Nothing is refused
 * here, so there is no all-or-nothing to preserve, and a mixed batch can have
 * its two orders picked out of the depth around them.
 *
 * @par Ordering
 * The outbox is a queue and the shipper drains it in order over one connection,
 * so a CANCEL written after a PLACE reaches the venue after it. That is not a
 * nicety: a cancel that overtook its own placement would be answered with
 * "unknown order" and the order it was meant to withdraw would stay working.
 *
 * @note Producer thread only, like every other link in this chain. It holds no
 *       atomics and the coroutine that empties the outbox runs on the same
 *       thread, which is what makes an ordinary @c std::vector the right
 *       hand-off here rather than one of @c core/concurrency's queues.
 */
template <class Sink>
class order_router {
public:
	using command = engine::event::command;

	/**
	 * @brief Requests that may wait to be sent before orders start being
	 *        dropped.
	 *
	 * A bound rather than an unbounded queue, because the two ends run at
	 * genuinely different speeds: a quoter requotes off every frame, ten a
	 * second, and a round trip to the venue is tens of milliseconds. A backlog
	 * that grows without limit turns a slow network into a memory leak and then
	 * into a flood of orders whose prices are minutes old - which is worse than
	 * dropping them, because a stale quote sent late is a real order at a price
	 * nobody meant.
	 *
	 * Sized for a few seconds of a busy quoter, so an ordinary hiccup is
	 * absorbed and a sustained one is reported.
	 */
	static constexpr std::size_t DEFAULT_OUTBOX_CAPACITY = 256;

	/**
	 * @brief A router in front of @p sink that sends nothing.
	 *
	 * The default state, and the one every command in this tree has run in so
	 * far: commands pass through untouched and no request is ever built.
	 * @ref attach is what makes it send.
	 *
	 * @param sink The far side. Must outlive the router.
	 * @param spec The listing, for turning ticks and lots back into the venue's
	 *        scaled decimals. Must outlive the router.
	 * @param venue_symbol The listing as the venue spells it. Copied - it is a
	 *        handful of bytes and a dangling one would be signed into a
	 * request.
	 * @param outbox_capacity @see DEFAULT_OUTBOX_CAPACITY
	 */
	order_router(Sink &sink, const engine::symbol_spec &spec,
				 std::string venue_symbol,
				 std::size_t outbox_capacity = DEFAULT_OUTBOX_CAPACITY)
		: sink_(&sink),
		  spec_(&spec),
		  venue_symbol_(std::move(venue_symbol)),
		  capacity_(outbox_capacity) {
		outbox_.reserve(outbox_capacity);
	}

	/**
	 * @brief Start sending, through @p gateway.
	 *
	 * @param gateway Builds and signs the requests. Borrowed; must outlive the
	 *        router.
	 *
	 * @par Why this is not a constructor argument
	 * Because a gateway asks a @c circuit_breaker whether an order may go, and
	 * in the live composition that breaker is a *member of the session this
	 * router is inside*. Neither object can therefore be built before the
	 * other, and something has to be late-bound. It is this direction rather
	 * than the breaker's because the two half-built states are not equally
	 * safe: a router with no gateway yet is exactly the default, and sends
	 * nothing, while a gateway with no breaker yet is a gateway that passes
	 * every order without asking - which is the failure worth making
	 * unreachable.
	 *
	 * @pre Called once, before anything is submitted. Attaching mid-run would
	 *      leave the orders already resting in the engine's book with no
	 *      counterpart at the venue and nothing to say so.
	 */
	void attach(venue_gateway &gateway) noexcept {
		assert(gateway_ == nullptr && "a router sends to one venue");
		assert(!venue_symbol_.empty() &&
			   "a gateway needs the listing's name at the venue");
		gateway_ = &gateway;
	}

	// --- the sink side, which is all the gate knows about -------------------

	/**
	 * @brief Hand @p batch downstream, and queue whatever of it is ours.
	 * @return Whatever the sink said. A refusal is the sink's - this layer
	 *         never manufactures one. @see the file header.
	 *
	 * @par Downstream first, and this is the whole correctness of the class
	 * A refused batch is *retried*, identically, by whoever wrote it - that is
	 * what @c risk_gate::submit_range being idempotent under back-pressure is
	 * for, and what @c live_session::quote does in a loop. A router that queued
	 * its requests before asking the sink would queue them again on every
	 * retry, and a run that stalled once would place the same order twice at
	 * the venue. Nothing downstream could detect that: the engine sees one
	 * command, and the second order is real, resting, and unaccounted for.
	 *
	 * So the sink decides first and nothing is queued unless it accepted. This
	 * layer is then idempotent under refusal for the same reason the gate is,
	 * which is the contract every link in this chain owes the one above it.
	 */
	[[nodiscard]] bool submit_range(std::span<const command> batch) {
		if (!sink_->submit_range(batch)) return false;
		if (gateway_ != nullptr)
			for (const command &cmd : batch) offer(cmd);
		return true;
	}

	// --- the shipper's side -------------------------------------------------

	/// @brief Whether anything is waiting to go out.
	[[nodiscard]] bool has_outbound() const noexcept {
		return !outbox_.empty();
	}

	/**
	 * @brief Take everything waiting, leaving the outbox empty.
	 *
	 * Moved out rather than borrowed, so the caller can @c co_await on it while
	 * the frame path keeps writing into a fresh one. A borrowed span would
	 * dangle the moment the next frame queued an order.
	 */
	[[nodiscard]] std::vector<outbound_request> take_outbound() {
		std::vector<outbound_request> taken;
		taken.reserve(capacity_);
		taken.swap(outbox_);
		outbox_.reserve(capacity_);
		return taken;
	}

	// --- what a report reads ------------------------------------------------

	/// @brief Whether this run can send at all. False is the default posture:
	///        the chain is spliced either way, so a report can say "nothing was
	///        sent" rather than being silent about it. @see order_router()
	[[nodiscard]] bool is_sending() const noexcept {
		return gateway_ != nullptr;
	}

	[[nodiscard]] router_stats stats() const noexcept {
		router_stats copy = stats_;
		copy.queued_now   = outbox_.size();
		return copy;
	}

	/// @brief The gateway, for its own counters and its weight budget. Null on
	///        a run that sends nothing.
	[[nodiscard]] venue_gateway *gateway() const noexcept { return gateway_; }

private:
	/// Build a request for one command, if it is one of ours and there is room.
	void offer(const command &cmd) {
		using enum engine::event::command_type;

		const bool is_ours = cmd.type == PLACE || cmd.type == CANCEL;
		if (!is_ours) return;
		++stats_.offered;

		// Checked before the gateway is asked, not after: building a request
		// debits the venue's weight budget, and spending weight on a request
		// that is then thrown away would have the run rate-limit itself out of
		// the market data it also needs. @see venue_gateway::place
		if (outbox_.size() >= capacity_) {
			++stats_.discarded;
			return;
		}

		// Two clocks, and neither is the session's. The venue's replay window
		// is a statement about *wall* time, so it needs a clock an operator and
		// the venue agree on; the weight budget measures an interval, so it
		// needs one that cannot step. `weight_budget` has already fixed its own
		// to steady_clock, so injecting the other half alone would buy nothing.
		// @see core/chrono/wall.hpp, which makes the same split for the
		// lifecycle records.
		const auto wall =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch())
				.count();
		const auto now = std::chrono::steady_clock::now();

		const auto built =
			cmd.type == PLACE
				? gateway_->place(cmd.as_place(),
								  *spec_,
								  venue_symbol_,
								  wall,
								  now)
				: gateway_->cancel(cmd.as_cancel(), venue_symbol_, wall, now);
		if (!built) {
			++stats_.refused;
			return;
		}
		outbox_.push_back(*built);
		++stats_.queued;
	}

	Sink *sink_;
	venue_gateway *gateway_ = nullptr;
	const engine::symbol_spec *spec_;
	std::string venue_symbol_;
	std::size_t capacity_;

	/// Written by the frame path, emptied by the shipper - both on the producer
	/// thread, which is why this is a plain vector. @see the class note.
	std::vector<outbound_request> outbox_;
	router_stats stats_{};
};

} // namespace exchange::session

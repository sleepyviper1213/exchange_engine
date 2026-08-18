#pragma once
// The return path's wire: one SPSC ring carrying published events from the
// matching thread back to the thread that generates orders.
//
// The command queue inside engine_partition is the forward half of the loop, and
// on its own it is a one-way street - submit() returns before any book has seen
// the command, so the producer learns nothing. This is the other half. It is a
// second queue and not a second use of the first because the two run in opposite
// directions between the same pair of threads, which is two SPSC relationships,
// not one.
//
// It adds no synchronisation of its own. Everything here is ordinary
// single-threaded code on one side or the other of spsc_queue's release store /
// acquire load, which is the single happens-before edge in the design: every
// event staged and copied into the ring by the engine thread is published by the
// release in publish_write, and becomes visible to the host thread through the
// matching acquire in its dequeue. See core/concurrency/lockfree/spsc_queue.hpp.

#include "core/concurrency/lockfree/spsc_queue.hpp"
#include "engine_event.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace exchange::engine::event {

/**
 * @brief The consumer-to-producer event channel: a ring plus the staging that
 *        makes filling it lossless.
 *
 * Two endpoints, one per thread, and which methods belong to which is the whole
 * contract:
 *
 * - **Engine side** (the matching thread, the ring's producer): @c publish,
 *   @c retry, @c has_pending, @c backlog.
 * - **Host side** (the order-generating thread, the ring's consumer):
 *   @c receive - or, in practice, an @c event_dispatcher wrapped around it.
 *
 * @c published and @c stalls belong to the engine side too: they are plain
 * integers the publishing thread owns, deliberately not atomics, because making
 * them readable from the other thread would put two stores on the return path to
 * serve a statistic. @c queued is the one observer either thread may call, and
 * it is a momentary snapshot, like the queue's own.
 *
 * @tparam Capacity Ring capacity in events; must be a power of two.
 *
 * @par Nothing is dropped, and that is a decision
 * A published trade is the venue's record that something happened. Losing one
 * because the strategy thread was momentarily behind would make the tape a
 * function of scheduling, which is the opposite of what the single-writer design
 * is for - so a full ring is back-pressure and never eviction. What the engine
 * thread must *not* do is block: it owns books that other people's orders are
 * waiting on. Those two together are why @c publish takes the batch by copy into
 * @c pending_ and hands back a boolean instead of blocking or discarding: the
 * batch is safe, the engine can go back to matching, and @c retry finishes the
 * job on a later turn of the loop.
 *
 * @code
 * // The consumer thread's loop body.
 * partition.drain();
 * if (!channel.publish(partition.runs(), partition.trades(),
 *                      partition.outcomes()))
 *     while (!channel.retry()) std::this_thread::yield();
 * partition.flush();
 * @endcode
 *
 * A caller happy to let the backlog ride to the next iteration can drop the
 * inner loop, so long as it calls @c retry before the next @c publish - which is
 * asserted, because publishing over a backlog is what would reorder the stream.
 *
 * @par Allocation
 * One, at construction: @c pending_ is reserved to @c Capacity, which is more
 * than a drain can usefully stage, and @c clear keeps that capacity. The staging
 * copy itself is not overhead the design added - a bare @c trade has to become an
 * @c engine_event somewhere contiguous before the ring's batch @c memcpy can take
 * it, so the copy is the conversion.
 */
// The default capacity lives on the declaration in fwd.hpp, which this header
// includes - repeating it here is a redefinition, not a restatement.
template <std::size_t Capacity>
class event_channel {
public:
	// Stated here rather than left to the ring's own assert so the diagnostic
	// names the capacity the caller actually chose. @see spsc_queue
	static_assert(std::has_single_bit(Capacity),
				  "Capacity must be a power of two");

	/// @brief Ring capacity in events, for a caller sizing its own buffers.
	static constexpr std::size_t CAPACITY = Capacity;

	event_channel() { pending_.reserve(Capacity); }

	// The ring is neither copyable nor movable and both endpoints hold this by
	// pointer; a channel lives where it was built, between two fixed threads.
	event_channel(const event_channel &)            = delete;
	event_channel &operator=(const event_channel &) = delete;
	event_channel(event_channel &&)                 = delete;
	event_channel &operator=(event_channel &&)      = delete;
	~event_channel()                                = default;

	/**
	 * @brief Engine side: stamp a drained batch with its listings and push it.
	 *
	 * @param runs The batch's cut list - @c engine_partition::runs(). Its last
	 *        entry's end offsets must cover both buffers; anything past them was
	 *        produced by no command and is not published.
	 * @param trades @c engine_partition::trades().
	 * @param outcomes @c engine_partition::outcomes().
	 * @return @c true when the whole batch reached the ring. On @c false the
	 *         remainder is held in @c pending_ and @c retry must clear it before
	 *         the next @c publish.
	 * @pre No backlog is outstanding (@c !has_pending()). Publishing over one
	 *      would put a newer batch in front of an older one, so this is asserted
	 *      rather than tolerated.
	 * @post Within a listing, the batch's trades precede its outcomes - the same
	 *       order @c flush publishes them in, for the same reason.
	 */
	bool publish(std::span<const symbol_run> runs,
				 std::span<const engine::trade> trades,
				 std::span<const engine::order_outcome> outcomes) {
		assert(!has_pending() &&
			   "clear the backlog with retry() before publishing a new batch");
		stage(runs, trades, outcomes);
		return drive();
	}

	/**
	 * @brief Engine side: push whatever a previous @c publish could not.
	 * @return @c true when the backlog is empty - vacuously so when there was
	 *         none, so this is safe to call unconditionally.
	 */
	bool retry() noexcept { return drive(); }

	/// @brief Engine side: whether events are staged but not yet in the ring.
	[[nodiscard]] bool has_pending() const noexcept {
		return cursor_ < pending_.size();
	}

	/// @brief Engine side: how many staged events are still waiting for room.
	[[nodiscard]] std::size_t backlog() const noexcept {
		return pending_.size() - cursor_;
	}

	/**
	 * @brief Host side: take up to @c out.size() events, oldest first.
	 * @param[out] out Contiguous, already-constructed destination.
	 * @return How many events were written to the front of @p out.
	 */
	template <class Rg>
		requires std::ranges::output_range<Rg, engine_event> &&
				 std::ranges::sized_range<Rg> &&
				 std::ranges::contiguous_range<Rg>
	[[nodiscard]] std::size_t receive(Rg &&out) noexcept {
		return queue_.try_dequeue_range(std::forward<Rg>(out));
	}

	/// @brief Engine side: events the ring has accepted since construction.
	[[nodiscard]] std::uint64_t published() const noexcept { return published_; }

	/// @brief Engine side: times a push found the ring full.
	///
	/// Not an error count - the events survived and @c retry will deliver them.
	/// It is the saturation signal for the *return* direction, and the mirror of
	/// @c strategy_engine::stalls: a channel that stalls steadily means the host
	/// thread cannot keep up with what the engine is publishing, which no amount
	/// of retrying fixes.
	[[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

	/// @brief Events currently in the ring - a momentary snapshot, readable from
	///        either side.
	[[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }

private:
	/// @brief Flatten (runs, trades, outcomes) into symbol-stamped events, in
	///        the order the host must see them.
	void stage(std::span<const symbol_run> runs,
			   std::span<const engine::trade> trades,
			   std::span<const engine::order_outcome> outcomes) {
		pending_.clear();
		cursor_                   = 0;
		std::size_t trade_begin   = 0;
		std::size_t outcome_begin = 0;
		for (const symbol_run &run : runs) {
			assert(run.trade_end <= trades.size() &&
				   run.outcome_end <= outcomes.size() &&
				   "a run's end offsets must lie inside the batch it cuts");
			assert(run.trade_end >= trade_begin &&
				   run.outcome_end >= outcome_begin &&
				   "a cut list's end offsets are monotonic");
			// Trades before outcomes, per listing: a fill and the order state it
			// produced arrive in the order that lets a reader apply the second to
			// the first.
			for (std::size_t i = trade_begin; i < run.trade_end; ++i)
				pending_.push_back(engine_event::of(run.symbol, trades[i]));
			for (std::size_t i = outcome_begin; i < run.outcome_end; ++i)
				pending_.push_back(engine_event::of(run.symbol, outcomes[i]));
			trade_begin   = run.trade_end;
			outcome_begin = run.outcome_end;
		}
	}

	/**
	 * @brief Push the staged suffix, in as many chunks as the ring has room for.
	 *
	 * The chunk size is measured against @c queue_.size(), which from the
	 * producer's side is an upper bound on how full the ring is - the consumer
	 * may have advanced since the load, never the other way - so the room this
	 * computes is a lower bound on the real thing. Conservative in the only
	 * direction that is safe: it can leave a slot unused for one iteration, and
	 * cannot overrun.
	 */
	bool drive() noexcept {
		while (cursor_ < pending_.size()) {
			const std::size_t room = Capacity - queue_.size();
			if (room == 0) [[unlikely]] {
				++stalls_;
				return false;
			}
			const std::size_t count = std::min(pending_.size() - cursor_, room);
			const std::span<const engine_event> chunk =
				std::span<const engine_event>(pending_).subspan(cursor_, count);
			if (!queue_.try_emplace_range(chunk)) [[unlikely]] {
				// Only reachable if the consumer un-advanced, which it cannot.
				++stalls_;
				return false;
			}
			cursor_ += count;
			published_ += count;
		}
		pending_.clear();
		cursor_ = 0;
		return true;
	}

	core::concurrency::lockfree::spsc_queue<engine_event, Capacity> queue_;

	/// Engine-side only: the current batch, flattened. Reserved once and reused,
	/// so a steady-state publish allocates nothing.
	std::vector<engine_event> pending_;
	std::size_t cursor_      = 0; ///< first staged event not yet in the ring
	std::uint64_t published_ = 0;
	std::uint64_t stalls_    = 0;
};

} // namespace exchange::engine::event

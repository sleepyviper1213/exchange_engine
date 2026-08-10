#pragma once
// A shard of the matching workload: a disjoint subset of listings, one command
// queue, one consumer thread.
//
// This is the unit of execution the architecture is built around. It owns every
// mutable thing the matching path touches — the queue, the books, the reusable
// batch buffers — so nothing inside it is shared with another partition and
// nothing needs a lock. Concurrency lives entirely in the queue.

#include "book_manager.hpp"
#include "core/concurrency/lockfree/spsc_queue.hpp"
#include "fwd.hpp"
#include "matching_engine.hpp"
#include "trading-engine/order_book.hpp"

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace exchange::engine::execution {

/**
 * @brief One partition: an SPSC command queue in front of the books it owns.
 *
 * The producer thread builds @c command%s and hands them off with @c submit /
 * @c submit_range and moves on — no trades exist yet. The consumer thread later
 * calls @c drain, which pops each command, has the @c matching_engine apply it
 * to the book its symbol names, and accumulates fills and lifecycle records
 * into buffers reused across drains. @c flush hands those to the sinks and
 * empties them.
 *
 * @par Why outcomes are mandatory rather than decoration
 * @c submit returns as soon as the command is enqueued, so the producer learns
 * nothing about what the book did with it — and cannot, since no book has seen
 * it yet. The outcome stream is the only channel carrying that answer back. It
 * is also where the cancel/fill race becomes observable: a CANCEL sitting in
 * the queue behind a PLACE that fills it arrives at a book with no such order
 * and reports CANCEL_REJECTED. Emporia models exactly this in
 * `OrderLifecycle.tla` as a pending cancel either confirmed or declined by a
 * winning fill.
 *
 * @par Registering listings
 * A partition executes only against listings it has been given. @c listing
 * creates one; a command for any other symbol is rejected rather than quietly
 * given a book, so a dispatcher and a reference-data set that disagree produce
 * a visible rejection instead of an order resting where nothing will ever match
 * it. Register at startup, on the consumer thread, before the producer begins.
 *
 * @tparam QueueCapacity Ring capacity; must be a power of two (spsc_queue).
 *
 * @par Threading contract
 * Exactly one producer thread calls @c submit / @c submit_range, and exactly
 * one consumer thread calls @c drain / @c flush / @c books / @c listing — the
 * single-producer, single-consumer rule the queue requires. No thread is
 * spawned; the caller owns both.
 */
// The default capacity lives on the declaration in fwd.hpp, which this header
// includes — repeating it here is a redefinition, not a restatement.
template <std::size_t QueueCapacity>
	requires (std::has_single_bit(QueueCapacity))
class engine_partition {
public:
	/// @brief Consumer-side callback fired by @c flush when the batch holds
	///        trades. The buffer is reused, so copy out anything kept past the
	///        call.
	using TradeSink = std::function<void(const std::vector<Trade> &)>;

	/// @brief The same for lifecycle records — acks, rejects, fills per order,
	///        and cancel confirmations.
	using OutcomeSink = std::function<void(const std::vector<order_outcome> &)>;

	/**
	 * @brief Construct a partition carrying no listings yet.
	 * @param on_trade Sink invoked by @c flush when trades were produced. May
	 * be empty to ignore trades.
	 * @param on_outcome Sink invoked by @c flush when lifecycle records were
	 *        produced. May be empty, which discards every ack, reject and
	 * cancel confirmation — appropriate for a benchmark, not for a venue with
	 *        clients.
	 * @param book_capacity Resting-order hint for each book @c listing creates.
	 * @param order_capacity Records the partition's @c order_manager holds. A
	 *        bound on simultaneously live orders *across every listing here*,
	 *        unlike @p book_capacity which is per book — one store serves the
	 *        whole partition, because a client order id is unique to the venue
	 *        and not to an instrument. Whatever is left over holds terminal
	 *        records, which is what lets a late cancel be told its order filled.
	 */
	explicit engine_partition(
		TradeSink on_trade, OutcomeSink on_outcome = {},
		std::size_t book_capacity   = book_manager::DEFAULT_BOOK_CAPACITY,
		std::uint32_t order_capacity = order_manager::DEFAULT_CAPACITY)
		: books_(book_capacity),
		  orders_(order_capacity),
		  engine_(books_, orders_),
		  on_trade_(std::move(on_trade)),
		  on_outcome_(std::move(on_outcome)) {}

	// The engine holds a pointer to books_, so neither copying nor moving a
	// partition would leave that pointer aimed at the right manager. A
	// partition is pinned to a thread anyway; there is nowhere for one to move
	// to.
	engine_partition(const engine_partition &)            = delete;
	engine_partition &operator=(const engine_partition &) = delete;
	engine_partition(engine_partition &&)                 = delete;
	engine_partition &operator=(engine_partition &&)      = delete;
	~engine_partition()                                   = default;

	/// @brief Give this partition responsibility for @p symbol, creating its
	///        book. Idempotent — a second call returns the existing book rather
	///        than discarding the orders resting on it.
	/// @return The listing's book, at an address that will not change.
	order_book &listing(symbol_id_t symbol) { return books_.create(symbol); }

	/**
	 * @brief Producer side: enqueue one command.
	 * @return @c false if the queue is full (lossless back-pressure — the
	 * caller retries or drops); @c true once enqueued.
	 */
	[[nodiscard]] bool submit(const command &cmd) noexcept {
		return queue_.try_emplace(cmd);
	}

	/**
	 * @brief Producer side: enqueue a whole batch, all-or-nothing.
	 * @return @c false if the batch did not fit; @c true once all enqueued.
	 */
	template <std::ranges::input_range Rg>
		requires std::convertible_to<std::ranges::range_reference_t<Rg>,
									 command>
	[[nodiscard]] bool submit_range(Rg &&batch) noexcept {
		return queue_.try_emplace_range(std::forward<Rg>(batch));
	}

	/**
	 * @brief Consumer side: apply every currently-queued command.
	 *
	 * Fills and lifecycle records land in the partition's buffers, which this
	 * clears first — so they hold exactly what this drain produced, and can be
	 * read straight off @c trades() / @c outcomes() by a consumer that
	 * installed no sinks. Nothing is published until @c flush; splitting the
	 * two lets a consumer drain a queue it is about to discard without telling
	 * anyone about it, and keeps "what happened" separate from "who was told".
	 *
	 * @return The number of commands applied, misroutes included — they came
	 * off the queue and were answered, they just did not reach a book.
	 */
	std::size_t drain() {
		trades_.clear();
		outcomes_.clear();
		std::size_t applied = 0;
		while (std::optional<command> cmd = queue_.try_dequeue()) {
			if (!engine_.process(*cmd, trades_, outcomes_)) ++misrouted_;
			++applied;
		}
		return applied;
	}

	/**
	 * @brief Consumer side: hand the accumulated batch to the sinks and empty
	 *        the buffers.
	 *
	 * The trade sink fires before the outcome sink, so a consumer reading both
	 * sees the executions before the order states that explain them. A sink is
	 * called only when its buffer is non-empty, so a flush with nothing to say
	 * costs nothing — and flushing twice publishes once, because the second
	 * call finds the buffers already empty.
	 */
	void flush() {
		if (!trades_.empty() && on_trade_) on_trade_(trades_);
		if (!outcomes_.empty() && on_outcome_) on_outcome_(outcomes_);
		trades_.clear();
		outcomes_.clear();
	}

	/// @brief Drain and publish in one step — the ordinary consumer loop body.
	/// @return The number of commands applied.
	std::size_t drain_and_flush() {
		const std::size_t applied = drain();
		flush();
		return applied;
	}

	/// @brief The listings this partition carries.
	[[nodiscard]] book_manager &books() noexcept { return books_; }

	[[nodiscard]] const book_manager &books() const noexcept { return books_; }

	/**
	 * @brief The venue's record of every order this partition has accepted,
	 *        including the ones its books have already finished with.
	 *
	 * Read it to answer "what happened to order 42" after the fact, or to check
	 * @c high_water() against the capacity the partition was built with. Clearing
	 * it is a session boundary: client order ids are unique within a session, and
	 * @c clear is what starts the next one — do it alongside the books, never on
	 * its own, or a live order would be resting with no record behind it.
	 */
	[[nodiscard]] order_manager &orders() noexcept { return orders_; }

	[[nodiscard]] const order_manager &orders() const noexcept {
		return orders_;
	}

	/// @brief Read access to one listing's book, or @c nullptr if this
	/// partition
	///        does not carry it.
	[[nodiscard]] const order_book *book(symbol_id_t symbol) const noexcept {
		return books_.lookup(symbol);
	}

	/// @brief The trades accumulated since the last @c flush. Reading these is
	///        the alternative to installing a sink.
	[[nodiscard]] const std::vector<Trade> &trades() const noexcept {
		return trades_;
	}

	/// @brief The lifecycle records accumulated since the last @c flush.
	[[nodiscard]] const std::vector<order_outcome> &outcomes() const noexcept {
		return outcomes_;
	}

	/**
	 * @brief Commands that named a listing this partition does not carry.
	 *
	 * Should be zero. Anything else means the dispatcher and the reference data
	 * disagree about who owns a symbol, which is a configuration fault rather
	 * than a market event — the affected clients got a rejection, but this
	 * counter is what says the deployment is wrong.
	 */
	[[nodiscard]] std::uint64_t misrouted() const noexcept {
		return misrouted_;
	}

private:
	// Declaration order is load-bearing: engine_ takes references to books_ and
	// orders_ at construction, so both must be built first and destroyed last.
	book_manager books_;
	order_manager orders_;
	matching_engine engine_;
	core::concurrency::lockfree::spsc_queue<command, QueueCapacity> queue_;
	std::vector<Trade> trades_;          ///< reused across drains
	std::vector<order_outcome> outcomes_; ///< reused across drains
	TradeSink on_trade_;
	OutcomeSink on_outcome_;
	std::uint64_t misrouted_ = 0;
};

} // namespace exchange::engine::execution

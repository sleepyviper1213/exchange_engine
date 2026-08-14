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
#include "core/metrics/counter.hpp"
#include "core/metrics/histogram.hpp"
#include "core/metrics/timer.hpp"
#include "fwd.hpp"
#include "matching_engine.hpp"
#include "trading-engine/event/engine_event.hpp"
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

// The cut list a drain produces is part of the event vocabulary, not of
// execution — a partition writes it and event::event_channel reads it. Pulled in
// unqualified the way matching_engine.hpp does with event::command, and for the
// same reason: it appears in this header's signatures.
using exchange::engine::event::symbol_run;

/**
 * @brief Optional, non-owning metrics a partition records into if given one.
 *
 * A plain pointer rather than a callback: TODO.md #12 already flags
 * @c TradeSink's @c std::function indirection as unwanted on a path budgeted
 * in nanoseconds, and a metrics hook runs on the same path, so this follows
 * the pointer shape instead of repeating that antipattern for a new one. The
 * owner constructs this alongside the partition and keeps it alive for the
 * partition's whole life — the same "single ownership" rule the rest of the
 * engine follows for mutable state, just applied to metrics too. See
 * core/metrics.hpp for what @c counter and @c histogram guarantee.
 */
struct partition_metrics {
	/// @brief Commands this partition has applied (drain() call count summed).
	core::metrics::counter commands_processed;
	/// @brief Trades this partition has published via flush().
	core::metrics::counter trades_emitted;
	/// @brief @copydoc engine_partition::misrouted
	core::metrics::counter misroutes;
	/// @brief Wall-clock time of each drain() call, in nanoseconds. One
	///        observation per batch, not per command — see
	///        core/metrics/timer.hpp on why that is the unit this can afford to
	///        time.
	core::metrics::histogram drain_latency_ns;
};

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
 * @par Getting those outcomes back to the producer
 * The sinks fire on the *consumer* thread, which is not where anything that
 * reacts to an outcome lives — a strategy host and a risk gate both sit on the
 * producer side, because that is the side that submits. Carrying the batch
 * across is @c event_channel's job, and @c runs() is the piece of the batch it
 * needs: a sink is handed trades and outcomes that name no listing, and a
 * partition holds many. @see event_channel, event_dispatcher
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
class engine_partition {
public:
	// Stated here rather than left to the queue's own assert so the diagnostic
	// names the capacity the caller actually chose. @see spsc_queue
	static_assert(std::has_single_bit(QueueCapacity),
				  "QueueCapacity must be a power of two");

	/// @brief Consumer-side callback fired by @c flush when the batch holds
	///        trades. The buffer is reused, so copy out anything kept past the
	///        call.
	using TradeSink = std::function<void(const std::vector<trade> &)>;

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
	 *        records, which is what lets a late cancel be told its order
	 * filled.
	 * @param metrics Where to record counters and drain latency, or @c nullptr
	 *        to record nothing — the default, so existing callers pay for
	 *        this only once they opt in. Must outlive the partition.
	 */
	explicit engine_partition(
		TradeSink on_trade, OutcomeSink on_outcome = {},
		std::size_t book_capacity    = book_manager::DEFAULT_BOOK_CAPACITY,
		std::uint32_t order_capacity = order_manager::DEFAULT_CAPACITY,
		partition_metrics *metrics   = nullptr)
		: books_(book_capacity),
		  orders_(order_capacity),
		  engine_(books_, orders_),
		  on_trade_(std::move(on_trade)),
		  on_outcome_(std::move(on_outcome)),
		  metrics_(metrics) {}

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
		runs_.clear();
		// One observation per drain, not per command — see partition_metrics
		// and core/metrics/timer.hpp on why the batch is the unit this can
		// afford to time. Guarded by metrics_ so an unmetered partition pays
		// for neither the clock read nor the histogram bump.
		std::optional<core::metrics::scoped_timer> timer;
		if (metrics_ != nullptr) timer.emplace(metrics_->drain_latency_ns);

		std::size_t applied = 0;
		while (std::optional<command> cmd = queue_.try_dequeue()) {
			if (!engine_.process(*cmd, trades_, outcomes_)) {
				++misrouted_;
				if (metrics_ != nullptr) metrics_->misroutes.increment();
			}
			record_run(cmd->symbol);
			++applied;
		}
		if (metrics_ != nullptr) metrics_->commands_processed.add(applied);
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
		if (!trades_.empty()) {
			if (on_trade_) on_trade_(trades_);
			if (metrics_ != nullptr)
				metrics_->trades_emitted.add(trades_.size());
		}
		if (!outcomes_.empty() && on_outcome_) on_outcome_(outcomes_);
		trades_.clear();
		outcomes_.clear();
		runs_.clear();
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
	 * @c high_water() against the capacity the partition was built with.
	 * Clearing it is a session boundary: client order ids are unique within a
	 * session, and
	 * @c clear is what starts the next one — do it alongside the books, never
	 * on its own, or a live order would be resting with no record behind it.
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
	[[nodiscard]] const std::vector<trade> &trades() const noexcept {
		return trades_;
	}

	/// @brief The lifecycle records accumulated since the last @c flush.
	[[nodiscard]] const std::vector<order_outcome> &outcomes() const noexcept {
		return outcomes_;
	}

	/**
	 * @brief Which listing produced which slice of @c trades() and
	 *        @c outcomes(), for the batch accumulated since the last @c flush.
	 *
	 * The piece that makes the batch routable. A partition carries many
	 * listings, and neither @c trade nor @c order_outcome names one — inside a
	 * book the listing is whichever book you are looking at, and that context
	 * does not survive being appended to a shared buffer. This is the context,
	 * kept beside the buffers rather than widened into every record: 12 bytes
	 * per listing per drain instead of 4 bytes per event, and no change to two
	 * types whose size the matching path cares about.
	 *
	 * Feed all three to @c event_channel::publish, which is the only thing that
	 * needs to read them, and do it *before* @c flush — flush empties the
	 * batch, these offsets included. @see symbol_run for how the slices are
	 * cut.
	 */
	[[nodiscard]] const std::vector<symbol_run> &runs() const noexcept {
		return runs_;
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

	/// @brief The metrics this partition was given, or @c nullptr if none.
	[[nodiscard]] partition_metrics *metrics() const noexcept {
		return metrics_;
	}

private:
	/**
	 * @brief Close off the cut list after one command, attributing whatever it
	 *        just appended to @p symbol.
	 *
	 * Called per command, so it is written to cost nothing when there is
	 * nothing to attribute: a command that produced neither a trade nor an
	 * outcome leaves the list alone, and a second command for the listing
	 * already at the back extends that entry instead of appending a new one. A
	 * partition whose flow is concentrated in a few names therefore ends a
	 * drain with a handful of runs, not one per command.
	 *
	 * The coalescing is safe because a run's meaning is "everything from the
	 * previous run's end to here, for this listing" — extending the back
	 * entry's ends is exactly that statement with a later "here".
	 */
	void record_run(symbol_id_t symbol) {
		const auto trade_end   = static_cast<std::uint32_t>(trades_.size());
		const auto outcome_end = static_cast<std::uint32_t>(outcomes_.size());
		// Where the last run left off — the start of the batch when there is no
		// last run, which is what makes the empty case need no separate branch.
		const std::uint32_t from_trade =
			runs_.empty() ? 0U : runs_.back().trade_end;
		const std::uint32_t from_outcome =
			runs_.empty() ? 0U : runs_.back().outcome_end;

		if (trade_end == from_trade && outcome_end == from_outcome)
			return; // this command published nothing; there is no slice to cut
		if (!runs_.empty() && runs_.back().symbol == symbol) {
			runs_.back().trade_end   = trade_end;
			runs_.back().outcome_end = outcome_end;
			return;
		}
		runs_.push_back({.symbol      = symbol,
						 .trade_end   = trade_end,
						 .outcome_end = outcome_end});
	}

	// Declaration order is load-bearing: engine_ takes references to books_ and
	// orders_ at construction, so both must be built first and destroyed last.
	book_manager books_;
	order_manager orders_;
	matching_engine engine_;
	core::concurrency::lockfree::spsc_queue<command, QueueCapacity> queue_;
	std::vector<trade> trades_;           ///< reused across drains
	std::vector<order_outcome> outcomes_; ///< reused across drains
	std::vector<symbol_run> runs_;        ///< reused across drains; @see runs()
	TradeSink on_trade_;
	OutcomeSink on_outcome_;
	std::uint64_t misrouted_    = 0;
	partition_metrics *metrics_ = nullptr; ///< non-owning; see the class note
};

} // namespace exchange::engine::execution

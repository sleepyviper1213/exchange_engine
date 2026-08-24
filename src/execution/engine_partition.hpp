#pragma once
// A shard of the matching workload: a disjoint subset of listings, one command
// queue, one consumer thread.
//
// This is the unit of execution the architecture is built around. It owns every
// mutable thing the matching path touches - the queue, the books, the reusable
// batch buffers - so nothing inside it is shared with another partition and
// nothing needs a lock. Concurrency lives entirely in the queue.

#include "book_manager.hpp"
#include "core/concurrency/lockfree/spsc_queue.hpp"
#include "core/persistence/record_log.hpp"
#include "core/metrics/counter.hpp"
#include "core/metrics/histogram.hpp"
#include "core/metrics/timer.hpp"
#include "fwd.hpp"
#include "matching_engine.hpp"
#include "event/engine_event.hpp"
#include "event/journal_record.hpp"
#include "order_book.hpp"

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace exchange::engine::execution {

// The cut list a drain produces is part of the event vocabulary, not of
// execution - a partition writes it and event::event_channel reads it. Pulled in
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
 * partition's whole life - the same "single ownership" rule the rest of the
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
	///        observation per batch, not per command - see
	///        core/metrics/timer.hpp on why that is the unit this can afford to
	///        time.
	core::metrics::histogram drain_latency_ns;
};

/**
 * @brief One partition: an SPSC command queue in front of the books it owns.
 *
 * The producer thread builds @c command%s and hands them off with @c submit /
 * @c submit_range and moves on - no trades exist yet. The consumer thread later
 * calls @c drain, which pops each command, has the @c matching_engine apply it
 * to the book its symbol names, and accumulates fills and lifecycle records
 * into buffers reused across drains. @c flush hands those to the sinks and
 * empties them.
 *
 * @par Why outcomes are mandatory rather than decoration
 * @c submit returns as soon as the command is enqueued, so the producer learns
 * nothing about what the book did with it - and cannot, since no book has seen
 * it yet. The outcome stream is the only channel carrying that answer back. It
 * is also where the cancel/fill race becomes observable: a CANCEL sitting in
 * the queue behind a PLACE that fills it arrives at a book with no such order
 * and reports CANCEL_REJECTED. Emporia models exactly this in
 * `OrderLifecycle.tla` as a pending cancel either confirmed or declined by a
 * winning fill.
 *
 * @par Getting those outcomes back to the producer
 * The sinks fire on the *consumer* thread, which is not where anything that
 * reacts to an outcome lives - a strategy host and a risk gate both sit on the
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
 * one consumer thread calls @c drain / @c flush / @c books / @c listing - the
 * single-producer, single-consumer rule the queue requires. No thread is
 * spawned; the caller owns both.
 */
// The default capacity lives on the declaration in fwd.hpp, which this header
// includes - repeating it here is a redefinition, not a restatement.
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

	/// @brief The same for lifecycle records - acks, rejects, fills per order,
	///        and cancel confirmations.
	using OutcomeSink = std::function<void(const std::vector<order_outcome> &)>;

	/**
	 * @brief Construct a partition carrying no listings yet.
	 * @param on_trade Sink invoked by @c flush when trades were produced. May
	 * be empty to ignore trades.
	 * @param on_outcome Sink invoked by @c flush when lifecycle records were
	 *        produced. May be empty, which discards every ack, reject and
	 * cancel confirmation - appropriate for a benchmark, not for a venue with
	 *        clients.
	 * @param book_capacity Resting-order hint for each book @c listing creates.
	 * @param order_capacity Records the partition's @c order_manager holds. A
	 *        bound on simultaneously live orders *across every listing here*,
	 *        unlike @p book_capacity which is per book - one store serves the
	 *        whole partition, because a client order id is unique to the venue
	 *        and not to an instrument. Whatever is left over holds terminal
	 *        records, which is what lets a late cancel be told its order
	 * filled.
	 * @param metrics Where to record counters and drain latency, or @c nullptr
	 *        to record nothing - the default, so existing callers pay for
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

	/// @brief The durable command log this partition writes, if it has one.
	using journal = core::persistence::record_log<event::journal_record>;

	/// @brief Give this partition responsibility for @p symbol, creating its
	///        book. Idempotent - a second call returns the existing book rather
	///        than discarding the orders resting on it.
	/// @return The listing's book, at an address that will not change.
	order_book &listing(symbol_id_t symbol) { return books_.create(symbol); }

	/**
	 * @brief Journal every command this partition applies from now on.
	 *
	 * @param log Where commands go, or @c nullptr to stop journalling. Not
	 *        owned; must outlive the partition or be detached first.
	 *
	 * @par What attaching one changes
	 * Two things, and the second is the one that matters. @c drain appends each
	 * command to @p log *before* handing it to the matching engine, so a command
	 * that changed a book is always in the log - the log can hold a command the
	 * books never saw (the process died in between), and recovery replaying it is
	 * how that heals, but it can never miss one they did. An append that fails
	 * therefore stops the drain instead of being counted and stepped over, since
	 * applying that command anyway is the one arrangement neither direction of
	 * recovery can repair. @see is_journal_faulted
	 *
	 * And @c flush becomes a durability barrier: it syncs the log before it
	 * publishes anything, and publishes nothing at all if the sync fails. That
	 * ordering is the whole point. A trade handed to a client whose command is
	 * still only in a buffer is a trade the venue may forget it made, and no
	 * amount of recovery afterwards can put that right - the client has already
	 * acted on it.
	 *
	 * @par Why the sync is per batch and not per command
	 * Because a durability barrier is a device round trip, which is hundreds of
	 * microseconds against a matching path budgeted in nanoseconds. Group commit
	 * is the standard answer and it costs nothing in correctness here: the batch
	 * is exactly the set of commands whose results @c flush is about to publish,
	 * so syncing once at the batch boundary makes every one of them durable
	 * before any of them is visible.
	 *
	 * A batch with nothing in it is not synced at all. An idle consumer polls
	 * @c drain_and_flush, so a barrier issued unconditionally would be a syscall
	 * per turn of a loop that had nothing to make durable - which is the same
	 * device round trip, spent on nothing.
	 *
	 * @par Threading
	 * Consumer side, like @c listing - call it before the producer starts. The
	 * log is written only by @c drain and @c flush, which is the consumer thread.
	 */
	void attach_journal(journal *log) {
		journal_ = log;
		// One drain can hold at most a full queue, so this is the largest batch
		// that can ever be staged. Reserved here rather than grown on the path:
		// attach_journal is a before-the-producer-starts call by contract, which
		// makes it the one place an allocation is free. A partition with no
		// journal never reserves and so pays nothing for the buffer at all.
		//
		// Both buffers, and for the same reason. The log frames each record with
		// its checksum before writing, which needs a staging buffer of its own;
		// telling it the worst case here is what keeps that buffer from growing
		// mid-drain and what keeps the batch leaving in a single fwrite.
		if (log != nullptr) {
			journal_batch_.reserve(QueueCapacity);
			journal_wire_.reserve(QueueCapacity);
			log->reserve(QueueCapacity);
		}
		// Nothing has been appended to *this* log yet, whatever was pending on
		// the last one. A dirty flag carried across a swap would either sync a
		// log that owes nothing or skip a barrier the new one needs.
		journal_dirty_ = false;
	}

	/// @brief The log this partition journals to, or @c nullptr.
	[[nodiscard]] journal *attached_journal() const noexcept { return journal_; }

	/**
	 * @brief Whether the journal failed and this partition has therefore stopped.
	 *
	 * Latched, and deliberately with no way to clear it. Once an append or a sync
	 * has failed, @c drain applies nothing further and @c flush publishes
	 * nothing: the partition can no longer promise that what it publishes was
	 * recorded, and no sequence of calls makes that untrue again. Attaching a
	 * fresh log does not help - the commands the old one lost are not in it.
	 *
	 * Stopping this way is what keeps the failure survivable. The queue fills and
	 * back-pressures the producer instead of the books running ahead of the log,
	 * and the batch @c flush withheld stays readable through @c trades() and
	 * @c outcomes(), because @c drain stops clearing the buffers once this is
	 * set.
	 *
	 * Recovering means a new partition replaying the store, which is the whole
	 * reason the store exists. @see core::persistence::event_store
	 */
	[[nodiscard]] bool is_journal_faulted() const noexcept {
		return journal_faulted_;
	}

	/**
	 * @brief Producer side: enqueue one command.
	 * @return @c false if the queue is full (lossless back-pressure - the
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
	 * clears first - so they hold exactly what this drain produced, and can be
	 * read straight off @c trades() / @c outcomes() by a consumer that
	 * installed no sinks. Nothing is published until @c flush; splitting the
	 * two lets a consumer drain a queue it is about to discard without telling
	 * anyone about it, and keeps "what happened" separate from "who was told".
	 *
	 * @return The number of commands applied, misroutes included - they came
	 * off the queue and were answered, they just did not reach a book.
	 */
	std::size_t drain() {
		// A faulted journal stops the partition rather than degrading it, and
		// stopping includes leaving the buffers alone: the batch flush withheld
		// is still in them, and it is the only account of what the books did with
		// the commands the log could not prove. Clearing it here - which is what
		// an unconditional drain did - turns a durability failure into silently
		// dropped trades. @see is_journal_faulted
		if (journal_faulted_) return 0;

		trades_.clear();
		outcomes_.clear();
		runs_.clear();
		// One observation per drain, not per command - see partition_metrics
		// and core/metrics/timer.hpp on why the batch is the unit this can
		// afford to time. Guarded by metrics_ so an unmetered partition pays
		// for neither the clock read nor the histogram bump.
		std::optional<core::metrics::scoped_timer> timer;
		if (metrics_ != nullptr) timer.emplace(metrics_->drain_latency_ns);

		std::size_t applied = 0;

		// Journalled partitions take the whole batch out first, record it in one
		// write, and only then apply it. Two reasons, and the second is why this
		// is not merely an optimisation.
		//
		// It is one fwrite per drain instead of one per command. That matters
		// more than it looks: an fwrite is a locking call on a FILE*, and the
		// per-call overhead measured at ~407ns against a drain that costs ~26ns
		// per command - so appending was 16x the cost of the matching it was
		// recording. See engine_partition_journal.bench.cpp for both numbers.
		//
		// And it makes the record all-or-nothing. Per-command appends could fail
		// half way through a batch, leaving a prefix journalled and the rest not;
		// one append either records the batch or records none of it, and the
		// commands are still in hand when that is decided, so none of them
		// reaches a book. That is a stronger version of the guarantee the
		// per-command path was reaching for. @see is_journal_faulted
		if (journal_ != nullptr) {
			// Bounded by the reservation, not by the queue running dry: the
			// producer is a different thread and may be refilling as this
			// drains, so an unbounded loop here could stage more than a full
			// queue and grow the buffer - a heap allocation on the matching
			// path, which is the one thing this file may not do. Anything past
			// the cap simply stays queued for the next drain, which is the same
			// answer back-pressure already gives.
			journal_batch_.clear();
			journal_wire_.clear();
			while (journal_batch_.size() < QueueCapacity) {
				std::optional<command> cmd = queue_.try_dequeue();
				if (!cmd) break;
				// Encoded as it is staged, so the batch is walked once rather
				// than twice. The commands are kept too: the journal takes the
				// encoded form and the books take the original, and re-decoding
				// what is already in hand would be work for nothing.
				journal_wire_.push_back(event::encode(*cmd));
				journal_batch_.push_back(*cmd);
			}
			if (journal_batch_.empty()) return 0;

			if (!journal_->append(
					std::span<const event::journal_record>(journal_wire_))) {
				++journal_failures_;
				journal_faulted_ = true;
				return 0; // recorded nothing, so apply nothing
			}
			journal_dirty_ = true;

			for (const command &cmd : journal_batch_) {
				if (!engine_.process(cmd, trades_, outcomes_)) {
					++misrouted_;
					if (metrics_ != nullptr) metrics_->misroutes.increment();
				}
				record_run(cmd.symbol);
				++applied;
			}
			if (metrics_ != nullptr) metrics_->commands_processed.add(applied);
			return applied;
		}

		while (std::optional<command> cmd = queue_.try_dequeue()) {
			// Journalled before it is applied, never after: a log missing a
			// command that changed a book cannot be replayed back to this state,
			// while a log holding one the books never saw replays harmlessly -
			// the command is simply applied during recovery instead. Only one of
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
	 * costs nothing - and flushing twice publishes once, because the second
	 * call finds the buffers already empty.
	 *
	 * @return @c false only when a journal is attached and could not be made
	 *         durable. Nothing was published in that case and the batch is still
	 *         in the buffers, and there is no retry that helps: the log is
	 *         poisoned, so the partition can no longer promise that what it
	 *         publishes has been recorded. It therefore stops rather than leaving
	 *         that to a caller who may be ignoring this return - every later
	 *         @c drain applies nothing and every later @c flush publishes
	 *         nothing. @see is_journal_faulted
	 * @note Returns @c true when no journal is attached, which is what makes this
	 *       a compatible change for every caller that ignores the result.
	 */
	bool flush() {
		if (journal_faulted_) return false;

		// Persist before you publish. Everything below this line is visible to
		// somebody outside the partition, so it must not run until the commands
		// that produced it are on the device - and if they cannot be, it must not
		// run at all. A trade a client has already acted on cannot be un-told.
		//
		// Only when there is something to persist, though. A barrier is a device
		// round trip, so an unconditional one puts a syscall in every turn of an
		// idle consumer loop - and "a flush with nothing to say costs nothing" is
		// a property this function documents rather than an accident of it.
		if (journal_ != nullptr && journal_dirty_) {
			if (!journal_->sync()) {
				++journal_failures_;
				journal_faulted_ = true;
				return false;
			}
			journal_dirty_ = false;
		}
		if (!trades_.empty()) {
			if (on_trade_) on_trade_(trades_);
			if (metrics_ != nullptr)
				metrics_->trades_emitted.add(trades_.size());
		}
		if (!outcomes_.empty() && on_outcome_) on_outcome_(outcomes_);
		trades_.clear();
		outcomes_.clear();
		runs_.clear();
		return true;
	}

	/// @brief Drain and publish in one step - the ordinary consumer loop body.
	/// @return The number of commands applied, which is zero for good once the
	///         journal has faulted. A failed durability barrier is deliberately
	///         not reported through this return - the partition stops itself, so
	///         a loop that polls this cannot silently trade through the failure,
	///         and one that has to *react* asks @c is_journal_faulted.
	std::size_t drain_and_flush() {
		const std::size_t applied = drain();
		(void)flush();
		return applied;
	}

	/**
	 * @brief Times the journal refused a write or a sync.
	 *
	 * Should be zero, and unlike @c misrouted it is not a configuration fault -
	 * it is the venue having lost its ability to promise durability. Non-zero
	 * means a command could not be recorded or a batch could not be made durable;
	 * either way the partition has already stopped, so this is the number that
	 * says *why* it stopped rather than a decision waiting to be taken.
	 *
	 * At most one failure is counted per cause, because the first one latches
	 * @c is_journal_faulted and nothing afterwards reaches the log.
	 */
	[[nodiscard]] std::uint64_t journal_failures() const noexcept {
		return journal_failures_;
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
	 * @c clear is what starts the next one - do it alongside the books, never
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
	 * listings, and neither @c trade nor @c order_outcome names one - inside a
	 * book the listing is whichever book you are looking at, and that context
	 * does not survive being appended to a shared buffer. This is the context,
	 * kept beside the buffers rather than widened into every record: 12 bytes
	 * per listing per drain instead of 4 bytes per event, and no change to two
	 * types whose size the matching path cares about.
	 *
	 * Feed all three to @c event_channel::publish, which is the only thing that
	 * needs to read them, and do it *before* @c flush - flush empties the
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
	 * than a market event - the affected clients got a rejection, but this
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
	 * previous run's end to here, for this listing" - extending the back
	 * entry's ends is exactly that statement with a later "here".
	 */
	void record_run(symbol_id_t symbol) {
		const auto trade_end   = static_cast<std::uint32_t>(trades_.size());
		const auto outcome_end = static_cast<std::uint32_t>(outcomes_.size());
		// Where the last run left off - the start of the batch when there is no
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
	journal *journal_           = nullptr; ///< non-owning; @see attach_journal
	std::uint64_t journal_failures_ = 0;
	std::vector<command> journal_batch_; ///< staged for one append; @see drain
	/// @brief The same batch encoded, which is what actually reaches the log.
	///
	/// A second buffer rather than encoding in place, because the two forms are
	/// both needed at once and for different consumers: the books apply the
	/// commands and the journal takes the records. Reserved alongside the first,
	/// so neither grows on the matching path.
	std::vector<event::journal_record> journal_wire_;
	bool journal_dirty_   = false; ///< appended to since the last barrier
	bool journal_faulted_ = false; ///< @see is_journal_faulted
};

} // namespace exchange::engine::execution

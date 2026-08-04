#pragma once
#include "core/concurrency/lockfree/spsc_queue.hpp"
#include "fwd.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book.hpp"

#include <concepts>
#include <functional>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace exchange::engine::execution {

// Downward dependencies: the engine consumes event::command and drives an
// order_book::OrderBook, appending order_book::Trade fills.
using exchange::engine::event::command;

/**
 * @brief Staged matching engine: an SPSC command queue in front of an
 * OrderBook.
 *
 * The producer thread builds @c Command%s and hands them off with @c submit /
 * @c submit_range and moves on — no trades exist yet. The consumer thread later
 * calls @c drain, which pops each command, applies it to the book (PLACE runs
 * @c match, appending fills to a reused trade buffer), and finally fires the
 * sinks once with the whole batch produced by that drain.
 *
 * @par Why the queue makes outcomes mandatory rather than nice to have
 * @c submit returns as soon as the command is enqueued, so the producer learns
 * nothing about what the book did with it — and cannot, since the book has not
 * seen it yet. The outcome stream is the only channel that carries that answer
 * back. It is also where the cancel/fill race becomes observable: a CANCEL
 * sitting in the queue behind a PLACE that fills it arrives at a book with no
 * such order, and reports CANCEL_REJECTED. Emporia models exactly this in
 * `OrderLifecycle.tla` as a pending cancel that either confirms or is declined
 * by a winning fill.
 *
 * @tparam QueueCapacity Ring capacity; must be a power of two (spsc_queue).
 *
 * @par Threading contract
 * Exactly one producer thread calls @c submit / @c submit_range and exactly one
 * consumer thread calls @c drain / @c book — the same single-producer,
 * single-consumer rule the underlying queue requires. No thread is spawned; the
 * caller owns both.
 */
template <std::size_t QueueCapacity>
	requires (std::has_single_bit(QueueCapacity))
class MatchingEngine {
public:
	/// @brief Consumer-side callback fired at the end of each @c drain that
	///        produced trades, with the batch of trades from that drain. The
	///        referenced buffer is reused, so copy out anything kept past the
	///        call.
	using TradeSink = std::function<void(const std::vector<Trade> &)>;

	/// @brief The same, for the lifecycle records that drain produced — acks,
	///        rejects, fills per order, and cancel confirmations.
	using OutcomeSink = std::function<void(const std::vector<OrderOutcome> &)>;

	/**
	 * @brief Construct the engine.
	 * @param on_trade Sink invoked on the consumer thread after a draining pass
	 *        that generated trades. May be empty to ignore trades.
	 * @param on_outcome Sink invoked after a draining pass that generated
	 *        lifecycle records. May be empty, which discards every ack, reject
	 *        and cancel confirmation — appropriate for a benchmark, not for a
	 *        venue with clients.
	 * @param book_capacity Max simultaneously resting orders (see OrderBook).
	 */
	explicit MatchingEngine(TradeSink on_trade, OutcomeSink on_outcome = {},
							std::size_t book_capacity = 1U << 10)
		: book_(book_capacity), on_trade_(std::move(on_trade)),
		  on_outcome_(std::move(on_outcome)) {}

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
	 * @brief Consumer side: apply every currently-queued command to the book,
	 *        then fire the sinks once each with everything produced this call.
	 *
	 * The trade sink fires before the outcome sink, so a consumer that reads
	 * both sees the executions before the order states they explain.
	 * @return The number of commands applied.
	 */
	std::size_t drain() {
		trades_.clear();
		outcomes_.clear();
		std::size_t applied = 0;
		while (std::optional<command> cmd = queue_.try_dequeue()) {
			apply(*cmd);
			++applied;
		}
		if (!trades_.empty() && on_trade_) on_trade_(trades_);
		if (!outcomes_.empty() && on_outcome_) on_outcome_(outcomes_);
		return applied;
	}

	/// @brief Consumer-side read access to the book (e.g. best_bid/best_ask).
	[[nodiscard]] const order_book &book() const noexcept { return book_; }

	/// @brief The records the last @c drain produced, valid until the next one.
	///        Reading these is the alternative to installing an outcome sink.
	[[nodiscard]] const std::vector<OrderOutcome> &outcomes() const noexcept {
		return outcomes_;
	}

private:
	void apply(const command &cmd) {
		switch (cmd.type) {
		case command::Type::PLACE:
			book_.place_order(cmd.order_, trades_, outcomes_);
			break;
		case command::Type::CANCEL:
			book_.cancel_order(cmd.cancel_id, outcomes_);
			break;
		case command::Type::ADD:
			book_.add_order(cmd.level.side, cmd.level.price, cmd.level.volume);
			break;
		case command::Type::REDUCE:
			book_.delete_order(cmd.level.side,
							   cmd.level.price,
							   cmd.level.volume);
			break;
		}
	}

	core::concurrency::lockfree::spsc_queue<command, QueueCapacity> queue_;
	exchange::engine::order_book book_;
	std::vector<Trade> trades_; ///< reused across drains — the trade buffer
	std::vector<OrderOutcome> outcomes_; ///< reused across drains
	TradeSink on_trade_;
	OutcomeSink on_outcome_;
};

} // namespace exchange::engine::execution

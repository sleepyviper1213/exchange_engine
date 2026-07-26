#pragma once
#include "core/concurrency/lockfree/spsc_queue.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book.hpp"
#include"fwd.hpp"
#include <concepts>
#include <functional>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>
namespace exchange::engine::execution {

// Downward dependencies: the engine consumes event::Command and drives an
// order_book::OrderBook, appending order_book::Trade fills.
using exchange::engine::event::Command;

/**
 * @brief Staged matching engine: an SPSC command queue in front of an
 * OrderBook.
 *
 * The producer thread builds @c Command%s and hands them off with @c submit /
 * @c submit_range and moves on — no trades exist yet. The consumer thread later
 * calls @c drain, which pops each command, applies it to the book (PLACE runs
 * @c match, appending fills to a reused trade buffer), and finally fires the
 * trade sink once with the whole batch produced by that drain.
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
class MatchingEngine {
public:
	/// @brief Consumer-side callback fired at the end of each @c drain that
	///        produced trades, with the batch of trades from that drain. The
	///        referenced buffer is reused, so copy out anything kept past the
	///        call.
	using TradeSink = std::function<void(const std::vector<Trade> &)>;

	/**
	 * @brief Construct the engine.
	 * @param on_trade Sink invoked on the consumer thread after a draining pass
	 *        that generated trades. May be empty to ignore trades.
	 * @param book_capacity Max simultaneously resting orders (see OrderBook).
	 */
	explicit MatchingEngine(TradeSink on_trade,
							std::size_t book_capacity = 1U << 10)
		: book_(book_capacity), on_trade_(std::move(on_trade)) {}

	/**
	 * @brief Producer side: enqueue one command.
	 * @return @c false if the queue is full (lossless back-pressure — the
	 * caller retries or drops); @c true once enqueued.
	 */
	[[nodiscard]] bool submit(const Command &cmd) noexcept {
		return queue_.try_emplace(cmd);
	}

	/**
	 * @brief Producer side: enqueue a whole batch, all-or-nothing.
	 * @return @c false if the batch did not fit; @c true once all enqueued.
	 */
	template <std::ranges::input_range Rg>
		requires std::convertible_to<std::ranges::range_reference_t<Rg>,
									 Command>
	[[nodiscard]] bool submit_range(Rg &&batch) noexcept {
		return queue_.try_emplace_range(std::forward<Rg>(batch));
	}

	/**
	 * @brief Consumer side: apply every currently-queued command to the book,
	 *        then fire the trade sink once with all trades produced this call.
	 * @return The number of commands applied.
	 */
	std::size_t drain() {
		trades_.clear();
		std::size_t applied = 0;
		while (std::optional<Command> cmd = queue_.try_dequeue()) {
			apply(*cmd);
			++applied;
		}
		if (!trades_.empty() && on_trade_) on_trade_(trades_);
		return applied;
	}

	/// @brief Consumer-side read access to the book (e.g. best_bid/best_ask).
	[[nodiscard]] const order_book &book() const noexcept { return book_; }

private:
	void apply(const Command &cmd) {
		switch (cmd.type) {
		case Command::Type::PLACE: book_.place_order(cmd.order, trades_); break;
		case Command::Type::CANCEL: book_.cancel_order(cmd.cancel_id); break;
		case Command::Type::ADD:
			book_.add_order(cmd.level.side, cmd.level.price, cmd.level.volume);
			break;
		case Command::Type::REDUCE:
			book_.delete_order(cmd.level.side,
							   cmd.level.price,
							   cmd.level.volume);
			break;
		case Command::Type::SET_LEVEL:
			book_.set_level(cmd.level.side, cmd.level.price, cmd.level.volume);
			break;
		}
	}

	core::concurrency::lockfree::spsc_queue<Command, QueueCapacity> queue_;
	exchange::engine::order_book book_;
	std::vector<Trade> trades_; ///< reused across drains — the trade buffer
	TradeSink on_trade_;
};

} // namespace execution

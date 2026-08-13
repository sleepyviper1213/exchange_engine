#pragma once
// Where a strategy puts the commands it decides to send.
//
// Two types, split along the template boundary on purpose. command_batch<N> owns
// the storage and knows its capacity, so N is a compile-time constant the host
// derives from the strategies it carries. command_writer is the cursor over that
// storage, and is *not* a template — so a strategy takes `command_writer &` and
// stays a plain class rather than becoming a template on somebody else's buffer
// size.

#include "core/util/start_lifetime_as.hpp"
#include "fwd.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <span>

namespace exchange::strategy {

/**
 * @brief A bounded output cursor a strategy writes commands into.
 *
 * @par Why the writer carries the symbol
 * Neither @c trade nor @c order_outcome names a listing — a trade is two order
 * ids, a price and a size, and an outcome is one order id and its state. So a
 * strategy fed from those streams cannot tell which instrument it is looking at,
 * and one that guessed would be wrong the moment its partition carried a second
 * listing. The resolution is to give the *host* the symbol and let it stamp
 * every command on the way out: a strategy names sides, prices and quantities
 * and never a symbol, and one host therefore observes exactly one listing.
 *
 * @par Capacity is a contract, not a runtime condition
 * Every strategy declares @c MAX_COMMANDS_PER_EVENT, the host sums them at
 * compile time, and it refuses to dispatch an event unless that many slots are
 * free. Overflow here is therefore a violated contract — a strategy emitting
 * more than it declared, or a hand-rolled caller skipping @c reserve — not
 * back-pressure, and it asserts rather than returning a status nobody could act
 * on. Hardened builds keep the assert (see @c enable_hardening).
 *
 * @note Trivially copyable, three pointers wide, and every write is a store plus
 *       an increment. Copy one when you want a save-point; the copy and the
 *       original then write to the same storage from wherever each was left.
 */
class command_writer {
public:
	/**
	 * @brief Build a cursor over @p first .. @p first + @p capacity.
	 * @param first Start of storage whose element lifetimes have begun.
	 * @param capacity Number of writable slots.
	 * @param symbol The listing every command written here is addressed to.
	 */
	constexpr command_writer(engine::event::command *first, std::size_t capacity,
							 symbol_id_t symbol) noexcept
		: begin_(first),
		  cur_(first),
		  end_(first + capacity),
		  symbol_(symbol) {}

	/// @brief Send @p o to the book, stamped with this writer's symbol.
	/// @note Takes the order by value because it rewrites @c symbol_id — a
	///       strategy's own copy is left alone.
	void place(engine::orders::order o) noexcept {
		o.symbol_id = symbol_;
		write(engine::event::command::place(o));
	}

	/// @brief Withdraw the resting order @p id.
	void cancel(order_id_t id) noexcept {
		write(engine::event::command::cancel(symbol_, id));
	}

	/// @brief Rest anonymous liquidity — no id, no matching, no outcomes.
	void add(side_t side, price_t price, quantity_t volume) noexcept {
		write(engine::event::command::add(symbol_, side, price, volume));
	}

	/// @brief Drain @p volume from a price level, oldest order first.
	void reduce(side_t side, price_t price, quantity_t volume) noexcept {
		write(engine::event::command::reduce(symbol_, side, price, volume));
	}

	/// @brief The commands written so far. Valid until @c reset.
	[[nodiscard]] std::span<const engine::event::command> written() const noexcept {
		return {begin_, cur_};
	}

	/// @brief Drop everything written; the storage is reused, not freed.
	constexpr void reset() noexcept { cur_ = begin_; }

	[[nodiscard]] constexpr std::size_t size() const noexcept {
		return static_cast<std::size_t>(cur_ - begin_);
	}

	/// @brief Slots still free. The host compares this against the compile-time
	///        per-event bound to decide whether to flush before the next event.
	[[nodiscard]] constexpr std::size_t remaining() const noexcept {
		return static_cast<std::size_t>(end_ - cur_);
	}

	[[nodiscard]] constexpr bool empty() const noexcept { return cur_ == begin_; }

	/// @brief The listing every command written here names.
	[[nodiscard]] constexpr symbol_id_t symbol() const noexcept {
		return symbol_;
	}

private:
	void write(const engine::event::command &cmd) noexcept {
		assert(cur_ != end_ &&
			   "command_writer overflow: a strategy emitted more commands for "
			   "one event than its MAX_COMMANDS_PER_EVENT promised, or the "
			   "caller dispatched without reserving room first");
		*cur_ = cmd;
		++cur_;
	}

	engine::event::command *begin_;
	engine::event::command *cur_;
	engine::event::command *end_;
	symbol_id_t symbol_;
};

/**
 * @brief Inline storage for @p Capacity commands, plus the cursor over it.
 *
 * No allocation, ever: the bytes are a member, and the capacity comes from a
 * compile-time sum over the strategies the host carries. A batch is filled
 * across several events and handed to the sink whole, so the queue sees one
 * @c try_emplace_range rather than one @c try_emplace per command.
 *
 * @tparam Capacity Number of commands the buffer holds. Must be at least one.
 *
 * @note Neither copyable nor movable, and cannot become either: @c writer_ holds
 *       pointers into @c storage_, so any relocation would leave a cursor aimed
 *       at the corpse. Same reasoning as @c execution::engine_partition — the
 *       object is pinned to the thread that drains its host, and there is
 *       nowhere for one to move to.
 */
template <std::size_t Capacity>
class command_batch {
	static_assert(Capacity > 0, "a batch with no room cannot accept a command");

public:
	/// @brief Storage for @p symbol's commands, empty and ready to write.
	explicit command_batch(symbol_id_t symbol) noexcept
		: writer_(core::util::start_lifetime_as_array<engine::event::command>(
					  storage_.data(), Capacity),
				  Capacity, symbol) {}

	command_batch(const command_batch &)            = delete;
	command_batch &operator=(const command_batch &) = delete;
	command_batch(command_batch &&)                 = delete;
	command_batch &operator=(command_batch &&)      = delete;
	~command_batch()                                = default;

	/// @brief The cursor. Hand this to a strategy.
	[[nodiscard]] command_writer &writer() noexcept { return writer_; }

	[[nodiscard]] const command_writer &writer() const noexcept {
		return writer_;
	}

	/// @brief What has accumulated since the last @c writer().reset().
	[[nodiscard]] std::span<const engine::event::command> view() const noexcept {
		return writer_.written();
	}

	[[nodiscard]] std::size_t size() const noexcept { return writer_.size(); }

	static constexpr std::size_t CAPACITY = Capacity;

private:
	// storage_ is declared first because writer_ points into it, and members
	// initialise in declaration order.
	//
	// Bytes rather than std::array<command, Capacity>: command has no default
	// constructor by design — a named factory picks the union's active member —
	// so an array of them cannot be default-initialised. start_lifetime_as_array
	// begins the lifetimes of trivially copyable objects over the bytes without
	// constructing anything, which is exactly the missing step, and leaves a
	// genuine array so a span over it is well-formed.
	alignas(engine::event::command)
		std::array<std::byte, Capacity * sizeof(engine::event::command)> storage_;
	command_writer writer_;
};

} // namespace exchange::strategy

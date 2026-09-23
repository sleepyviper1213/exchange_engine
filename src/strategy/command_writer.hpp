#pragma once
// Where a strategy puts the commands it decides to send.
//
// Two types, split along the template boundary on purpose. command_batch<N>
// owns the storage and knows its capacity, so N is a compile-time constant the
// host derives from the strategies it carries. command_writer is the cursor
// over that storage, and is *not* a template - so a strategy takes
// `command_writer &` and stays a plain class rather than becoming a template on
// somebody else's buffer size.

#include "core/util/attributes.hpp"
#include "event/command.hpp"
#include "fwd.hpp"
#include "orders/order.hpp"
#include "orders/types.hpp"
#include "strategy_export.hpp"

#include <cassert>
#include <cstddef>
#include <span>

namespace exchange::strategy {

/**
 * @brief A bounded output cursor a strategy writes commands into.
 *
 * @par Why the writer carries the symbol
 * Neither @c trade nor @c order_outcome names a listing - a trade is two order
 * ids, a price and a size, and an outcome is one order id and its state. So a
 * strategy fed from those streams cannot tell which instrument it is looking
 * at, and one that guessed would be wrong the moment its partition carried a
 * second listing. The resolution is to give the *host* the symbol and let it
 * stamp every command on the way out: a strategy names sides, prices and
 * quantities and never a symbol, and one host therefore observes exactly one
 * listing.
 *
 * @par Capacity is a contract, not a runtime condition
 * Every strategy declares @c MAX_COMMANDS_PER_EVENT, the host sums them at
 * compile time, and it refuses to dispatch an event unless that many slots are
 * free. Overflow here is therefore a violated contract - a strategy emitting
 * more than it declared, or a hand-rolled caller skipping @c reserve - not
 * back-pressure, and it asserts rather than returning a status nobody could act
 * on. Hardened builds keep the assert (see @c enable_hardening).
 *
 * @note Trivially copyable, three pointers wide, and every write is a store
 * plus an increment. Copy one when you want a save-point; the copy and the
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
	STRATEGY_EXPORT command_writer(engine::event::command *first,
								   std::size_t capacity,
								   symbol_id_t symbol) noexcept;

	/// @brief Send @p o to the book, stamped with this writer's symbol.
	/// @note Takes the order by value because it rewrites @c symbol_id - a
	///       strategy's own copy is left alone.
	STRATEGY_EXPORT void place(engine::orders::order o) noexcept;

	/// @brief Withdraw the resting order @p id.
	STRATEGY_EXPORT void cancel(order_id_t id) noexcept;

	/// @brief Amend the resting order @p id onto @p price and @p quantity.
	///
	/// One command where a withdraw-and-replace is two, and - where the price
	/// does not move and the quantity falls - the only way to resize without
	/// giving up queue position. @see engine::order_book::modify_order for what
	/// each shape of amendment costs in priority.
	///
	/// @param quantity The new *order* quantity, counted from inception and
	///        including what has filled. @see engine::orders::amendment
	STRATEGY_EXPORT void modify(order_id_t id, price_t price,
								quantity_t quantity,
								timestamp_t at = 0) noexcept;

	/// @brief Rest anonymous liquidity - no id, no matching, no outcomes.
	STRATEGY_EXPORT void add(side_t side, price_t price,
							 quantity_t volume) noexcept;

	/// @brief Drain @p volume from a price level, oldest order first.
	STRATEGY_EXPORT void reduce(side_t side, price_t price,
								quantity_t volume) noexcept;

	/// @brief The commands written so far. Valid until @c reset.
	[[nodiscard]] STRATEGY_EXPORT std::span<const engine::event::command>
	written() const noexcept EXCHANGE_LIFETIMEBOUND;

	/// @brief Drop everything written; the storage is reused, not freed.
	STRATEGY_EXPORT void reset() noexcept;

	[[nodiscard]] STRATEGY_EXPORT std::size_t size() const noexcept;

	/// @brief Slots still free. The host compares this against the compile-time
	///        per-event bound to decide whether to flush before the next event.
	[[nodiscard]] STRATEGY_EXPORT std::size_t remaining() const noexcept;

	[[nodiscard]] STRATEGY_EXPORT bool empty() const noexcept;

	/// @brief The listing every command written here names.
	[[nodiscard]] STRATEGY_EXPORT symbol_id_t symbol() const noexcept;

private:
	void write(const engine::event::command &cmd) noexcept;

	engine::event::command *begin_;
	engine::event::command *cur_;
	engine::event::command *end_;
	symbol_id_t symbol_;
};

} // namespace exchange::strategy

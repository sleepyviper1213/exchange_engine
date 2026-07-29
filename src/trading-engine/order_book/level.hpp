#pragma once
#include "detail/order_list.hpp"
#include "fwd.hpp"
#include "order.hpp"

namespace exchange::engine {

/// @brief One price level: a price and the FIFO of orders resting at it.
///
/// Orders are held oldest-first (front() fills first) as an intrusive list of
/// pool nodes, so the level itself is a flat aggregate of scalars owning no
/// storage of its own. That is what lets book_side shift a whole side with one
/// memmove when a level is inserted or erased, and what keeps a level's
/// aggregate qty an O(1) read rather than a walk over its orders.
struct Level {
	price price;
	detail::order_list orders;

	/// @brief Rest @p order at this level, allocating its node from @p pool.
	TRADING_ENGINE_EXPORT void add_order(detail::order_pool &pool,
										 const Order &order);

	/// @brief True when no orders rest at this level.
	[[nodiscard]] TRADING_ENGINE_EXPORT bool has_empty_orders() const noexcept;

	/// @brief Sum of the resting orders' volumes. O(1) — the list keeps it.
	[[nodiscard]] TRADING_ENGINE_EXPORT quantity total_volume() const noexcept;

	/// @brief How many orders rest here. O(1).
	///
	/// Exported alongside total_volume so out-of-DLL readers (the formatter)
	/// go through Level rather than reaching into the unexported order_list.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t order_count() const noexcept;
};

} // namespace exchange::engine

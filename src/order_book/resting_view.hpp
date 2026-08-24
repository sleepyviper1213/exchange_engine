#pragma once
// One resting order, described as a value - what a traversal of the book hands
// out, and what a snapshot writes down.
//
// The book's internal representation of a resting order is a pool cell that is
// also an intrusive list node (`detail::resting_order`), and it must stay that
// way: the matching loop reads an order's id and quantity out of the same cache
// line it followed the list pointer through. None of that survives being written
// to a file, and none of it is anybody else's business. This is the same order
// with the structure taken off.

#include "order_state.hpp"
#include "orders/side.hpp"
#include "orders/types.hpp"

#include <type_traits>

namespace exchange::engine {

/**
 * @brief A resting order as a plain value: where it sits, and how far through
 *        its life it is.
 *
 * @par Why it carries the whole @c order_state and not just a remaining quantity
 * Because the cumulative traded quantity is not recoverable from anything else,
 * and losing it is silent. An order that has filled 4 of 10 and is restored as a
 * fresh order of 6 rests correctly, quotes correctly and matches correctly - and
 * then reports its next fill as 2-of-6 to a client who has been told 4-of-10.
 * The state is 8 bytes and it is the difference between a snapshot that restores
 * a book and one that restores a book while quietly rewriting its history.
 *
 * @note Trivially copyable, so a snapshot of these is a raw append through
 *       @c core::persistence::record_log with no encode step. @see book_snapshot
 */
struct resting_view {
	order_id_t id;      ///< the client's id, or zero for anonymous liquidity
	order_state state;  ///< quantity, traded and therefore remaining
	price_t price;      ///< the level it rests at
	side_t side;        ///< which side of the book that level is on

	bool operator==(const resting_view &) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<resting_view>,
			  "a resting_view is snapshotted as its object representation");

/// @brief The snapshot's stride, pinned - and for the same reason
///        @c event::command's is. @see execution::resting_record
static_assert(sizeof(resting_view) == 24,
			  "a snapshot's record layout changed; existing snapshots will be "
			  "refused by record_log's stride check");

} // namespace exchange::engine

#pragma once
// What a pool does when it runs out - the one policy knob `order_pool` takes.
//
// Split from order_pool.hpp so the choice can be named without the pool: the
// book states it at construction, and a test states it to prove the refusal
// path.

namespace exchange::engine::detail {

/// @brief What a pool does when asked for more cells than it was sized for.
enum class pool_growth : bool {
	/// @brief Chain another block. Correct, but the @c acquire that triggers it
	///        pays for the whole block inline - and on the matching path that
	///        is
	///        a millisecond-scale stall in the middle of a crossing order.
	chained,
	/// @brief Refuse: @c acquire returns @c nullptr once @c capacity cells are
	///        live. Latency stays flat and the refusal reaches the client as a
	///        CANCELLED / BOOK_AT_CAPACITY outcome, which is what that reason
	///        code exists to say.
	fixed,
};

} // namespace exchange::engine::detail

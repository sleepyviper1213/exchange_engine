#pragma once
// Command execution: what one command does to one book.
//
// Nothing about queues, threads, batching or publication lives here. That is
// engine_partition's job, and the split is what lets this be a pure function of
// (command, books) — the same command against the same books always does the
// same thing, which is the property replay and verification rest on.

#include "book_manager.hpp"
#include "fwd.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book.hpp"

#include <vector>

namespace exchange::engine::execution {

// Downward dependencies: the engine consumes event::command and drives the
// order_book that book_manager owns, appending Trade fills and OrderOutcome
// lifecycle records.
using exchange::engine::event::command;

/**
 * @brief Applies commands to the books a @c book_manager owns.
 *
 * Holds no book of its own — it looks one up per command, which is the whole
 * reason one partition can serve many listings. Holds no queue and no buffers
 * either: the records a command produces are appended to buffers the caller
 * supplies, so a partition can reuse one pair across a whole drain and publish
 * them as a single batch.
 *
 * @note Not thread-safe, and cannot be: the books it drives are single-writer
 *       by contract. One partition, one consumer thread, one engine.
 */
class matching_engine {
public:
	/// @brief Execute against the listings @p books carries.
	/// @param books Must outlive the engine — a partition owns both, and
	///        declares the manager first so it does.
	TRADING_ENGINE_EXPORT explicit matching_engine(book_manager &books) noexcept;

	/**
	 * @brief Apply one command to the book its symbol names.
	 *
	 * @par A command for a listing this partition does not carry
	 * Refused, never created on demand. Reaching the wrong partition means the
	 * dispatcher and the reference data disagree, and resting the order on a
	 * book no other command will ever address would hide that behind an order
	 * that simply never fills. So it is rejected with @c UNKNOWN_SYMBOL: an
	 * identified PLACE gets a REJECTED, a CANCEL gets a CANCEL_REJECTED, and
	 * anonymous depth — ADD, REDUCE, and a PLACE under the reserved id 0 —
	 * produces no record because there is nobody to report to. The return value
	 * says so in every case.
	 *
	 * @param cmd The command; routed by @c command::symbol whatever its type.
	 * @param trades Fills are appended here; never cleared.
	 * @param outcomes Lifecycle records are appended here; never cleared.
	 * @return @c true if a book took the command, @c false if the symbol has no
	 *         book on this partition.
	 */
	TRADING_ENGINE_EXPORT bool process(const command &cmd,
									   std::vector<Trade> &trades,
									   std::vector<OrderOutcome> &outcomes);

	/// @brief The listings this engine executes against.
	[[nodiscard]] TRADING_ENGINE_EXPORT book_manager &books() const noexcept;

private:
	/// @brief Record that @p cmd named a listing this partition does not carry.
	static void reject_misrouted(const command &cmd,
								 std::vector<OrderOutcome> &outcomes);

	/// Pointer rather than reference, so the engine stays assignable — a
	/// partition holding one by value should not lose copy assignment over it.
	book_manager *books_;
};

} // namespace exchange::engine::execution

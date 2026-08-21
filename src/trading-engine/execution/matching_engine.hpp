#pragma once
// Command execution: what one command does to one book.
//
// Nothing about queues, threads, batching or publication lives here. That is
// engine_partition's job, and the split is what lets this be a pure function of
// (command, books) - the same command against the same books always does the
// same thing, which is the property replay and verification rest on.

#include "execution_export.hpp" // EXECUTION_EXPORT (generated)
#include "book_manager.hpp"
#include "fwd.hpp"
#include "order_manager.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book.hpp"

#include <cstddef>
#include <vector>

namespace exchange::engine::execution {

// Downward dependencies: the engine consumes event::command and drives the
// order_book that book_manager owns, appending trade fills and order_outcome
// lifecycle records.
using exchange::engine::event::command;

/**
 * @brief Applies commands to the books a @c book_manager owns.
 *
 * Holds no book of its own - it looks one up per command, which is the whole
 * reason one partition can serve many listings. Holds no queue and no buffers
 * either: the records a command produces are appended to buffers the caller
 * supplies, so a partition can reuse one pair across a whole drain and publish
 * them as a single batch.
 *
 * @par Two stores, and why the engine drives both
 * The book holds *resting* orders; the @c order_manager holds *every* order the
 * venue accepted, including the ones the book has finished with. Keeping them
 * in step is this class's job and nowhere else's, because this is the only
 * place that sees a whole command's effect: it admits the order before the book
 * gets it, and then reconciles the manager from the outcome stream the book
 * emitted.
 *
 * Reconciling from the outcomes rather than mirroring each step by hand is the
 * decision that keeps the two from drifting. The outcome stream is already the
 * canonical account of what happened - it names both sides of every fill and
 * carries absolute traded/remaining quantities - so a record built from it
 * cannot disagree with what the client was told. A second hand-written mirror
 * could, and the divergence would only surface as a client's position not
 * adding up.
 *
 * @note Not thread-safe, and cannot be: the books it drives are single-writer
 *       by contract. One partition, one consumer thread, one engine.
 */
class matching_engine {
public:
	/**
	 * @brief Execute against the listings @p books carries, recording every
	 *        order in @p orders.
	 * @param books Must outlive the engine - a partition owns both, and
	 *        declares the manager first so it does.
	 * @param orders The venue's record store. Same lifetime requirement, and
	 *        sized to worst-case live orders: a full one refuses new orders
	 *        with @c BOOK_AT_CAPACITY rather than forgetting a live one.
	 */
	EXECUTION_EXPORT matching_engine(book_manager &books,
										  order_manager &orders) noexcept;

	/**
	 * @brief Apply one command to the book its symbol names.
	 *
	 * @par A PLACE the record store refuses
	 * Refused before the book sees it, and the book stays untouched. That
	 * covers a quantity the lifecycle cannot represent, an id already spent -
	 * which here means still resting *or* still remembered, a stricter test
	 * than the book's own - and a record store with no room for another live
	 * order. Each arrives as a REJECTED carrying its own reason.
	 *
	 * @par A CANCEL for an order the book no longer has
	 * The book answers CANCEL_REJECTED / UNKNOWN_ORDER for every such request,
	 * because its index holds only resting orders and cannot tell "filled a
	 * microsecond ago" from "never placed". This upgrades that reason from the
	 * record store, so the client is told ORDER_ALREADY_FILLED,
	 * ORDER_ALREADY_CANCELLED or ORDER_ALREADY_REJECTED where the record
	 * survives. UNKNOWN_ORDER remains the answer when it genuinely does not -
	 * never placed, or aged out of the store's history.
	 *
	 * @note ADD and REDUCE cannot put the two stores out of step, and it is
	 *       @c order_book::delete_order that guarantees it rather than anything
	 *       here: a reduction drains anonymous depth only and walks past an
	 *       identified order rather than destroying one silently. Depth commands
	 *       therefore never touch a record, which is why they need no
	 *       reconciliation.
	 *
	 * @par A command for a listing this partition does not carry
	 * Refused, never created on demand. Reaching the wrong partition means the
	 * dispatcher and the reference data disagree, and resting the order on a
	 * book no other command will ever address would hide that behind an order
	 * that simply never fills. So it is rejected with @c UNKNOWN_SYMBOL: an
	 * identified PLACE gets a REJECTED, a CANCEL gets a CANCEL_REJECTED, and
	 * anonymous depth - ADD, REDUCE, and a PLACE under the reserved id 0 -
	 * produces no record because there is nobody to report to. The return value
	 * says so in every case.
	 *
	 * @param cmd The command; routed by @c command::symbol whatever its type.
	 * @param trades Fills are appended here; never cleared.
	 * @param outcomes Lifecycle records are appended here; never cleared.
	 * @return @c true if a book took the command, @c false if the symbol has no
	 *         book on this partition.
	 */
	EXECUTION_EXPORT bool process(const command &cmd,
									   std::vector<trade> &trades,
									   std::vector<order_outcome> &outcomes);

	/// @brief The listings this engine executes against.
	[[nodiscard]] EXECUTION_EXPORT book_manager &books() const noexcept;

	/// @brief The venue's record of every order this engine has accepted.
	[[nodiscard]] EXECUTION_EXPORT order_manager &orders() const noexcept;

private:
	/// @brief Record that @p cmd named a listing this partition does not carry.
	static void reject_misrouted(const command &cmd,
								 std::vector<order_outcome> &outcomes);

	/// @brief Admit @p incoming, match it, and bring its record up to date.
	void place(order_book &book, const orders::order &incoming,
			   std::vector<trade> &trades, std::vector<order_outcome> &outcomes);

	/// @brief Apply a cancel for @p id, answering it from the record store when
	///        the book cannot.
	void cancel(order_book &book, order_id_t id,
				std::vector<order_outcome> &outcomes);

	/**
	 * @brief Apply @p outcomes from index @p first onwards to the record store.
	 *
	 * Every outcome, not just the aggressor's: a fill names the resting order
	 * too, and its record has to move with it. That is why this walks the
	 * buffer and looks each id up rather than acting on the one handle @c place
	 * has in hand - the maker's handle is not in hand anywhere.
	 */
	void reconcile(const std::vector<order_outcome> &outcomes,
				   std::size_t first);

	/// Pointers rather than references, so the engine stays assignable - a
	/// partition holding one by value should not lose copy assignment over it.
	book_manager *books_;
	order_manager *orders_;
};

} // namespace exchange::engine::execution

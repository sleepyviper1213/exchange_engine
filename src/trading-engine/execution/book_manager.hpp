#pragma once
// Per-symbol order book ownership.
//
// The matching engine executes commands; this owns the books they execute
// against. Splitting the two is what lets one partition serve many instruments
// without the engine growing a notion of "which book" — it asks, and gets a
// reference back.

#include "fwd.hpp"
#include "trading-engine/order_book/order_book.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace exchange::engine::execution {

/**
 * @brief Creates, owns, finds and destroys the @c order_book of each listing a
 *        partition is responsible for.
 *
 * @par Why a vector of pointers rather than a map of books
 * Two constraints pulling the same way. @c symbol_id_t is dense by contract —
 * reference data assigns the ids — so the natural index is the id itself, and a
 * lookup is one bounds check and one load rather than a hash and a probe. That
 * matters because @c lookup sits on the per-command path, once per dispatch.
 *
 * The indirection is not a choice: @c order_book owns intrusive ladders whose
 * links point at levels it holds, so it deletes its move constructor and cannot
 * live in a vector that relocates. Holding each behind a @c unique_ptr also buys
 * the property the engine needs anyway — a book's address never changes, so a
 * reference taken from @c lookup stays valid however many listings are added
 * afterwards.
 *
 * @note The density contract is real: @c create(1'000'000) sizes the slot vector
 *       to a million pointers. That is 8 MB of mostly-null, and it is the price
 *       of the id being an index. Ids come from reference data, which assigns
 *       them consecutively; a venue with sparse ids wants a map here instead.
 *
 * @note Not thread-safe, deliberately. One partition, one thread, one manager —
 *       the same single-owner rule the books themselves rest on.
 */
class book_manager {
public:
	/// @brief Resting-order capacity given to each book that does not name one.
	///        Passed straight to @c order_book's own capacity hint.
	static constexpr std::size_t DEFAULT_BOOK_CAPACITY = 1U << 15;

	TRADING_ENGINE_EXPORT explicit book_manager(
		std::size_t default_book_capacity = DEFAULT_BOOK_CAPACITY) noexcept;

	/**
	 * @brief The book for @p symbol, creating it if this listing is new.
	 *
	 * Idempotent, and that is a safety property rather than a convenience: a
	 * @c create that replaced an existing book would destroy every order resting
	 * on it silently, with no outcome to tell the clients who owned them. A
	 * second call for a live listing therefore returns the book already there.
	 * Use @c remove when destruction is actually what is meant.
	 *
	 * @param symbol The listing's dense id.
	 * @param capacity Resting-order hint for a book created by this call;
	 *        ignored when one already exists.
	 * @return The listing's book, at an address that will not change.
	 */
	TRADING_ENGINE_EXPORT order_book &create(symbol_id_t symbol,
											 std::size_t capacity);

	/// @brief Create with the manager's default capacity.
	/// @see create(symbol_id_t, std::size_t)
	TRADING_ENGINE_EXPORT order_book &create(symbol_id_t symbol);

	/**
	 * @brief The book for @p symbol, or @c nullptr if the listing has none.
	 *
	 * Null rather than a book created on demand: a command naming a symbol this
	 * partition does not carry is a routing error, and answering it with an
	 * empty book would turn a misroute into an order silently accepted onto a
	 * book nobody reads. The caller rejects it instead.
	 */
	[[nodiscard]] TRADING_ENGINE_EXPORT order_book *
	lookup(symbol_id_t symbol) noexcept;

	/// @brief The book for @p symbol, or @c nullptr. @see lookup(symbol_id_t)
	[[nodiscard]] TRADING_ENGINE_EXPORT const order_book *
	lookup(symbol_id_t symbol) const noexcept;

	/// @brief Whether @p symbol has a book here.
	[[nodiscard]] TRADING_ENGINE_EXPORT bool
	contains(symbol_id_t symbol) const noexcept;

	/**
	 * @brief Destroy @p symbol's book and everything resting on it.
	 *
	 * @warning No @c order_outcome is emitted for the orders that go with it, for
	 *          the same reason @c order_book::clear emits none: this is a
	 *          listing being delisted or a partition torn down, not a market
	 *          being withdrawn. Cancel the orders first if anyone is owed a
	 *          report.
	 * @return @c true if a book was there and is now gone.
	 */
	TRADING_ENGINE_EXPORT bool remove(symbol_id_t symbol) noexcept;

	/// @brief How many listings have a book. Not the slot count — ids arriving
	///        out of order leave holes, and a hole is not a book.
	[[nodiscard]] TRADING_ENGINE_EXPORT std::size_t size() const noexcept;

	/// @brief Whether no listing has a book.
	[[nodiscard]] TRADING_ENGINE_EXPORT bool empty() const noexcept;

	/// @brief Destroy every book. The slot vector keeps its storage.
	/// @warning Emits no outcomes. @see remove
	TRADING_ENGINE_EXPORT void clear() noexcept;

private:
	/// Indexed by symbol id. A null slot is a listing this partition does not
	/// carry, which is the same answer as an id past the end.
	std::vector<std::unique_ptr<order_book> > books_;
	std::size_t live_ = 0; ///< non-null slots, so size() is not a count_if
	std::size_t default_book_capacity_;
};

} // namespace exchange::engine::execution

#pragma once
// Which of our orders are still out there, and for how much.
//
// The gate cannot compute exposure from filled position alone: an account
// holding nothing while showing a thousand orders is one adverse print away
// from holding all of it. So something has to remember every order between the
// moment it is submitted and the moment it is finished, and this is that
// something.

#include "fwd.hpp"
#include "trading-engine/orders/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace exchange::risk {

/// @brief One order the ledger is still tracking.
struct working_order {
	order_id_t id;   ///< the client id it was submitted under
	side_t side;     ///< which way it would take the account
	price_t price;   ///< limit price, in ticks
	quantity_t lots; ///< quantity still working
};

/// @brief What came out of the ledger when quantity was taken from an entry.
struct ledger_take {
	side_t side;          ///< the entry's side, for the counter to decrement
	price_t price;        ///< the entry's limit price, in ticks
	quantity_t taken;     ///< lots actually removed, clamped to what was there
	quantity_t remaining; ///< lots still working; zero means the entry is gone
};

/**
 * @brief An open-addressed table of the orders this account has in flight.
 *
 * @par Why not @c order_manager, which already records every order
 * Because that one lives in @c execution/ and belongs to the partition's
 * *consumer* thread, and this is read and written by the *producer* thread
 * before a command has reached a queue, let alone a book. Reaching across for
 * it would be a risk check depending on the thing it is supposed to gate — an
 * edge pointing the wrong way through the very boundary this module defines.
 * The duplication is four fields, and it buys the gate the property that it can
 * answer entirely from state it owns.
 *
 * @par Layout
 * Linear probing over a power-of-two array, sized once at construction and
 * never grown. Each slot is sixteen bytes — four to a cache line — which is why
 * the side is folded into the sign of the stored quantity rather than kept as
 * its own byte: a bool would round the slot up to twenty-four and cut probe
 * locality by a third for information that is already there. @c working_order
 * is the unpacked view, returned by value; nothing outside sees the encoding.
 *
 * Order id zero is the empty marker and costs nothing to reserve, because the
 * book already treats it as the anonymous sentinel — an order carrying it rests
 * without an index and produces no outcomes, so it could never be tracked here
 * anyway. @see orders::order::id
 *
 * @par Deletion
 * Backward-shift, not tombstones. A venue session is long and a tombstoned
 * table degrades monotonically: every cancelled order leaves a marker that
 * lengthens every later probe until the table is rebuilt, so a strategy quoting
 * all day would watch its risk check get slower by the hour. Backward-shift
 * keeps the table in the state it would have been in had the entry never been
 * inserted, which costs a short loop on erase and nothing at all afterwards.
 *
 * @par Threading
 * None. One producer thread inserts, reduces and retires; no atomics, no
 * synchronisation, and none wanted. The aggregate the *outside* world needs —
 * total working lots per side — is mirrored into @c position_book, which is
 * where the sharing is paid for once.
 */
class working_ledger {
public:
	/// @brief Smallest table the ledger will build, so a tiny limit still
	/// probes
	///        sanely.
	static constexpr std::size_t MIN_SLOTS = 8;

	/**
	 * @brief A ledger holding up to @p max_orders at once.
	 *
	 * The table is over-allocated to keep the load factor near 0.7: linear
	 * probing degrades sharply past that, and the memory is a few tens of
	 * kilobytes against a check that has to stay in the single-digit
	 * nanoseconds.
	 */
	explicit working_ledger(std::uint32_t max_orders);

	/// @brief Orders currently tracked.
	[[nodiscard]] std::uint32_t size() const noexcept;

	/// @brief Most orders that may be tracked at once.
	[[nodiscard]] std::uint32_t limit() const noexcept;

	/// @brief Table slots allocated — always a power of two, always more than
	///        @c limit().
	[[nodiscard]] std::size_t slot_count() const noexcept;

	[[nodiscard]] bool empty() const noexcept;

	/// @brief Whether another order would fit.
	[[nodiscard]] bool full() const noexcept;

	/// @brief Whether @p id is being tracked.
	[[nodiscard]] bool contains(order_id_t id) const noexcept;

	/// @brief What is working under @p id, if anything.
	[[nodiscard]] std::optional<working_order>
	find(order_id_t id) const noexcept;

	/**
	 * @brief Start tracking @p lots of @p id at @p price on @p side.
	 *
	 * @return @c false if @p id is already tracked, the ledger is at @c
	 * limit(),
	 *         @p id is the reserved zero, or @p lots is not positive. All four
	 *         are breaches the gate reports rather than conditions it recovers
	 *         from — see @c breach::DUPLICATE_ORDER and
	 *         @c breach::WORKING_ORDERS.
	 */
	[[nodiscard]] bool insert(order_id_t id, side_t side, price_t price,
							  quantity_t lots) noexcept;

	/**
	 * @brief Remove up to @p lots from @p id's working quantity.
	 *
	 * The entry is erased when nothing is left, so a fully filled order stops
	 * being tracked without a second call.
	 *
	 * @param lots Quantity to take. Clamped to what is there: a fill larger
	 * than the ledger thinks is working means the ledger missed something, and
	 *        taking the entry to a negative would corrupt every later exposure
	 *        check rather than only this one.
	 * @return What was taken, or @c nullopt if @p id is not tracked.
	 */
	[[nodiscard]] std::optional<ledger_take> take(order_id_t id,
												  quantity_t lots) noexcept;

	/**
	 * @brief Stop tracking @p id entirely, whatever is left of it.
	 *
	 * What a cancel or a rejection does: the order is finished and every lot it
	 * still had working is no longer exposure.
	 *
	 * @return What was still working, or @c nullopt if @p id is not tracked.
	 */
	[[nodiscard]] std::optional<ledger_take> retire(order_id_t id) noexcept;

	/// @brief Forget everything. A session boundary, not a recovery step.

	void clear() noexcept;

private:
	/// @brief Sixteen bytes: id, price, and the quantity carrying the side in
	///        its sign. @see the class note on layout.
	struct slot {
		order_id_t id          = 0;
		price_t price          = 0;
		quantity_t signed_lots = 0;
	};

	static_assert(sizeof(slot) == 16,
				  "a ledger slot must stay at four to a cache line");

	static constexpr std::size_t NOT_FOUND = static_cast<std::size_t>(-1);

	/// @brief Positive is a bid, negative an ask. @pre @p lots is positive.
	[[nodiscard]] static quantity_t pack(side_t side, quantity_t lots) noexcept;

	[[nodiscard]] static working_order unpack(const slot &s) noexcept;

	/**
	 * @brief Where @p id would like to live.
	 *
	 * Fibonacci hashing — one multiply and one shift. Client order ids are
	 * usually a dense ascending run, which the identity hash would scatter
	 * perfectly and a modulo-prime would too; the multiply is here for the case
	 * that is not true, where ids are strided by session or by venue and the
	 * low bits are constant. Taking the *high* bits of the product is what
	 * makes every input bit matter.
	 */
	[[nodiscard]] std::size_t home(order_id_t id) const noexcept;

	[[nodiscard]] std::size_t find_slot(order_id_t id) const noexcept;

	/**
	 * @brief Empty @p at and pull back any entry a probe would now miss.
	 *
	 * Knuth's algorithm 6.4R. Emptying a slot in a linear-probing table breaks
	 * every chain that ran through it, so each following entry is examined and
	 * moved back if — and only if — its ideal position is not inside the span
	 * that is being reorganised. The scan stops at the first genuinely empty
	 * slot, which bounds it by the cluster rather than by the table.
	 */
	void erase_at(std::size_t at) noexcept;

	std::uint32_t limit_;
	std::vector<slot> slots_;
	std::size_t mask_;
	unsigned shift_;
	std::uint32_t size_ = 0;
};

} // namespace exchange::risk

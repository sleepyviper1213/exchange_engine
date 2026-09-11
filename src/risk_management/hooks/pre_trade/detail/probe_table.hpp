#pragma once
// The open-addressed table `working_ledger` is built on, and the probing that
// goes with it.
//
// Split from the ledger because the two answer different questions. The ledger
// decides what an order *means* - how a side folds into the sign of a quantity,
// when an entry is finished, what a caller is told - and this decides where a
// row lives and how it is found again. Keeping the second out of the public
// header is what leaves `working_ledger` showing an interface instead of a hash
// table.

#include "orders/types.hpp"

#include <cstddef>
#include <vector>

namespace exchange::risk::hooks::pre_trade::detail {

/**
 * @brief One row: id, price, and the quantity carrying the side in its sign.
 *
 * Sixteen bytes - four to a cache line - which is why the side is folded into
 * the sign of the stored quantity rather than kept as its own byte: a @c bool
 * would round the slot up to twenty-four and cut probe locality_hint by a third for
 * information that is already there. @c working_order is the unpacked view,
 * returned by value; nothing outside the ledger sees this encoding.
 *
 * Order id zero is the empty marker and costs nothing to reserve, because the
 * book already treats it as the anonymous sentinel - an order carrying it rests
 * without an index and produces no outcomes, so it could never be tracked here
 * anyway. @see orders::order::id
 */
struct ledger_slot {
	order_id_t id          = 0;
	price_t price          = 0;
	quantity_t signed_lots = 0;
};

static_assert(sizeof(ledger_slot) == 16,
			  "a ledger slot must stay at four to a cache line");

/**
 * @brief Linear probing over a power-of-two array, sized once and never grown.
 *
 * @par Deletion
 * Backward-shift, not tombstones. A venue session is long and a tombstoned
 * table degrades monotonically: every cancelled order leaves a marker that
 * lengthens every later probe until the table is rebuilt, so a strategy quoting
 * all day would watch its risk check get slower by the hour. Backward-shift
 * keeps the table in the state it would have been in had the entry never been
 * inserted, which costs a short loop on erase and nothing at all afterwards.
 *
 * @warning Every probe loop here terminates on an empty slot and on nothing
 *          else. The table must therefore never be filled: @c working_ledger
 *          over-allocates to a load factor near 0.7 and refuses at @c limit()
 *          long before that matters, and no other caller is intended.
 */
class probe_table {
public:
	/// @brief The answer to a lookup that found nothing.
	static constexpr std::size_t NOT_FOUND = static_cast<std::size_t>(-1);

	/// @param slot_count Slots to allocate. @pre a power of two, at least two.
	explicit probe_table(std::size_t slot_count);

	/// @brief Slots allocated - always a power of two.
	[[nodiscard]] std::size_t slot_count() const noexcept;


	[[nodiscard]] const ledger_slot &operator[](std::size_t at) const noexcept;

	[[nodiscard]] ledger_slot &operator[](std::size_t at) noexcept;

	/// @brief Where @p id is, or @c NOT_FOUND. @pre @p id is not zero.
	[[nodiscard]] std::size_t find(order_id_t id) const noexcept;

	/**
	 * @brief The slot @p id would be written to.
	 *
	 * One probe answers both questions an insert has: it walks the chain from
	 * @p id's home and stops either at the entry that is already there or at
	 * the first free slot after it. Asking separately would walk the same
	 * cluster twice.
	 *
	 * @return The free slot to claim, or @c NOT_FOUND if @p id is already in
	 *         the table. @pre @p id is not zero.
	 */
	[[nodiscard]] std::size_t vacancy_for(order_id_t id) const noexcept;

	/**
	 * @brief Empty @p at and pull back any entry a probe would now miss.
	 *
	 * Knuth's algorithm 6.4R. Emptying a slot in a linear-probing table breaks
	 * every chain that ran through it, so each following entry is examined and
	 * moved back if - and only if - its ideal position is not inside the span
	 * that is being reorganised. The scan stops at the first genuinely empty
	 * slot, which bounds it by the cluster rather than by the table.
	 *
	 * @note Counting is the ledger's job: this leaves the table one entry
	 *       lighter and says nothing about how many are left.
	 */
	void erase(std::size_t at) noexcept;

	/// @brief Empty every slot, keeping the allocation.
	void clear() noexcept;

private:
	/**
	 * @brief Where @p id would like to live.
	 *
	 * Fibonacci hashing - one multiply and one shift. Client order ids are
	 * usually a dense ascending run, which the identity hash would scatter
	 * perfectly and a modulo-prime would too; the multiply is here for the case
	 * that is not true, where ids are strided by session or by venue and the
	 * low bits are constant. Taking the *high* bits of the product is what
	 * makes every input bit matter.
	 */
	[[nodiscard]] std::size_t home(order_id_t id) const noexcept;

	std::vector<ledger_slot> slots_;
	std::size_t mask_;
	unsigned shift_;
};

} // namespace exchange::risk::hooks::pre_trade::detail

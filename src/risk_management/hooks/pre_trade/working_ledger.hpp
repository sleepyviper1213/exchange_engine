#pragma once
// Which of our orders are still out there, and for how much.
//
// The gate cannot compute exposure from filled position alone: an account
// holding nothing while showing a thousand orders is one adverse print away
// from holding all of it. So something has to remember every order between the
// moment it is submitted and the moment it is finished, and this is that
// something.

#include "detail/probe_table.hpp"
#include "risk_management_export.hpp"
#include "orders/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace exchange::risk::hooks::pre_trade {

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
 * it would be a risk check depending on the thing it is supposed to gate - an
 * edge pointing the wrong way through the very boundary this module defines.
 * The duplication is four fields, and it buys the gate the property that it can
 * answer entirely from state it owns.
 *
 * @par Layout
 * Linear probing over a power-of-two array, sized once at construction and
 * never grown - @see detail::probe_table, which is where the probing, the slot
 * encoding and the backward-shift deletion live. This class is the part that
 * has an opinion about orders: what a side means, when an entry is finished,
 * and what a caller is told about it.
 *
 * @par Threading
 * None. One producer thread inserts, reduces and retires; no atomics, no
 * synchronisation, and none wanted. The aggregate the *outside* world needs -
 * total working lots per side - is mirrored into @c position_book, which is
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
	RISK_MANAGEMENT_EXPORT explicit working_ledger(std::uint32_t max_orders);

	/// @brief Orders currently tracked.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint32_t size() const noexcept;

	/// @brief Most orders that may be tracked at once.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint32_t limit() const noexcept;

	/// @brief Table slots allocated - always a power of two, always more than
	///        @c limit().
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::size_t
	slot_count() const noexcept;

	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool is_empty() const noexcept;

	/// @brief Whether another order would fit.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool is_full() const noexcept;

	/// @brief Whether @p id is being tracked.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	contains(order_id_t id) const noexcept;

	/// @brief What is working under @p id, if anything.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::optional<working_order>
	find(order_id_t id) const noexcept;

	/**
	 * @brief Start tracking @p lots of @p id at @p price on @p side.
	 *
	 * @return @c false if @p id is already tracked, the ledger is at @c
	 * limit(),
	 *         @p id is the reserved zero, or @p lots is not positive. All four
	 *         are breaches the gate reports rather than conditions it recovers
	 *         from - see @c breach::DUPLICATE_ORDER and
	 *         @c breach::WORKING_ORDERS.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	insert(order_id_t id, side_t side, price_t price, quantity_t lots) noexcept;

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
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::optional<ledger_take>
	take(order_id_t id, quantity_t lots) noexcept;

	/**
	 * @brief Stop tracking @p id entirely, whatever is left of it.
	 *
	 * What a cancel or a rejection does: the order is finished and every lot it
	 * still had working is no longer exposure.
	 *
	 * @return What was still working, or @c nullopt if @p id is not tracked.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::optional<ledger_take>
	retire(order_id_t id) noexcept;

	/// @brief Forget everything. A session boundary, not a recovery step.
	RISK_MANAGEMENT_EXPORT void clear() noexcept;

private:
	std::uint32_t limit_;
	detail::probe_table table_;
	std::uint32_t size_ = 0;
};

} // namespace exchange::risk::hooks::pre_trade
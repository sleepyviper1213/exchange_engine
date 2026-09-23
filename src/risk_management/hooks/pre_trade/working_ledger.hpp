#pragma once
// Which of our orders are still out there, and for how much.
//
// The gate cannot compute exposure from filled position alone: an account
// holding nothing while showing a thousand orders is one adverse print away
// from holding all of it. So something has to remember every order between the
// moment it is submitted and the moment it is finished, and this is that
// something.

#include "detail/probe_table.hpp"
#include "orders/types.hpp"
#include "risk_management_export.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace exchange::risk::hooks::pre_trade {

/// @brief One order the ledger is still tracking.
struct working_order {
	order_id_t id;   ///< the client id it was submitted under
	side_t side;     ///< which way it would take the account
	price_t price;   ///< limit price, in ticks
	quantity_t lots; ///< quantity still working
};

/**
 * @brief Where a @ref working_ledger::snapshot stopped, to resume from.
 *
 * Opaque: only @c snapshot interprets it, and a caller's only correct use is to
 * pass back the one it was handed. It is a struct rather than a bare index so
 * that it cannot be confused with the count beside it in @ref ledger_scan, and
 * so that the table's addressing stays the ledger's business.
 */
struct ledger_cursor {
	std::size_t at = 0;
};

/// @brief What one @ref working_ledger::snapshot copied out, and where to
///        carry on from.
struct ledger_scan {
	std::size_t written = 0; ///< Entries written to the caller's buffer.
	ledger_cursor next{};    ///< Pass back to continue; meaningless once
							 ///< @c written is zero.
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
	 * @brief Copy tracked orders into @p out, resuming from @p from.
	 *
	 * The only way to enumerate the ledger, and the one a mass cancel is built
	 * on: everything else here is keyed by id, which is no help to a caller
	 * whose whole problem is that it does not know the ids.
	 *
	 * @param out Buffer to fill. At most @c out.size() entries are written.
	 * @param from Where to start; default-constructed begins at the first slot.
	 * @return How many were written, and the cursor to pass back for the rest.
	 *         A @c written of zero means the walk is finished.
	 *
	 * @par Why a buffer and a cursor rather than an iterator or a callback
	 * Both alternatives would have to put @c ledger_slot - the sixteen-byte
	 * encoding with the side folded into the sign of a quantity - in this
	 * header, either as the iterator's value type or as the callback's
	 * parameter through an inline template. That encoding is sealed in
	 * @c working_ledger.cpp on purpose, and this keeps it there: the loop and
	 * the unpacking are both on the far side of a non-template function, and
	 * the caller only ever sees @ref working_order.
	 *
	 * The cursor is what lets a caller walk a full ledger through a small fixed
	 * buffer, so a mass cancel neither allocates nor sizes a member against
	 * @c limit(). @see risk_gate::mass_cancel
	 *
	 * @warning Slot order, which is a hash order - not insertion order, not
	 *          price order, and not stable across inserts. Nothing that needs
	 *          an ordering should use this without imposing its own.
	 *
	 * @warning A cursor is invalidated by any @c insert, @c take, @c retire or
	 *          @c clear, because backward-shift deletion moves entries between
	 *          slots. Finish a walk before mutating, or start it again.
	 *
	 * @warning An empty @p out writes nothing and therefore reports the walk
	 *          finished, which for a caller looping until @c written is zero is
	 *          an immediate exit rather than an error. Passing one is a caller
	 *          bug and is left as one: the check would cost a branch on every
	 *          chunk to catch a call nobody has reason to make.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT ledger_scan snapshot(
		std::span<working_order> out, ledger_cursor from = {}) const noexcept;

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
	 * @brief Move @p id onto @p price and @p lots, whatever it was on before.
	 *
	 * What an amendment does to the ledger, and the one mutator that can make
	 * an entry *bigger*. @c insert and @c take between them can only add an
	 * order or shrink one, because until there was a MODIFY command nothing
	 * could grow a working quantity without a new id.
	 *
	 * @return The entry as it was, or @c nullopt when @p id is not tracked or
	 *         @p lots is not positive. The caller needs the previous quantity
	 *         to move the same delta into the position book, and nothing else
	 *         here remembers it.
	 *
	 * @warning This does not erase an entry the way @c take does when nothing
	 *          is left. An amendment to zero lots is not representable - the
	 *          book turns one into a cancel - so @p lots must be positive, and
	 *          an order that is finished is @c retire's business.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::optional<working_order>
	amend(order_id_t id, price_t price, quantity_t lots) noexcept;

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
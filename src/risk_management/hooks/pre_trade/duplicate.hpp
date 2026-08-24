#pragma once
// Duplication / State Hook: an id already working, and the ledger's ceiling.
//
// The rule that catches the failure mode a size check cannot see - a strategy
// re-sending the same order because it never processed the ack. Each copy is
// individually reasonable; the loop is what is wrong, and the only evidence of
// it is that the id is already working.
//
// It is also the one pre-trade rule that is not pure arithmetic, and that is
// why it lives in its own file rather than beside the size rules: reserving an
// id is a hash probe, which is a loop, so it is the one check the gate
// deliberately keeps behind a branch and runs *last* - an order that already
// failed a limit never touches the table at all.

// Exported because the caller is `risk_gate`, a template instantiated in the
// consumer's own translation unit - the definition lives in duplicate.cpp on
// this side of the library boundary, so the symbol has to be visible across it.
#include "risk_management_export.hpp"
#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/pre_trade/working_ledger.hpp"
#include "orders/order.hpp"

namespace exchange::risk::hooks::pre_trade {

/**
 * @brief Reserve @p o in @p ledger, or say why it cannot be.
 *
 * @return @c WORKING_ORDERS when the ledger is at its ceiling,
 *         @c DUPLICATE_ORDER when the id is already working, and zero when the
 *         reservation succeeded.
 *
 * @warning **This mutates on success.** A zero return means @p o is now in the
 *          ledger and its lots count as exposure for every later command in the
 *          batch. The caller owns the undo: if the batch is not delivered,
 *          @c risk_gate::roll_back retires exactly the ids that claimed
 *          cleanly, which is what makes @c submit_range idempotent under
 *          back-pressure. Every clean PLACE in a batch was inserted *by that
 *          batch - a pre-existing id would have come back as @c DUPLICATE_ORDER
 *          - so the undo needs no bookkeeping beyond the masks it already has.
 *
 * @par Why the ceiling is a risk rule and not an allocation failure
 * The ledger is sized once, from @c risk_limits::max_working_orders, and never
 * grows: the no-heap-on-the-ingest-path invariant means a full table cannot be
 * answered by allocating a bigger one. So "too many orders working at once" is
 * a limit an operator sets, reported like any other. @see working_ledger
 *
 * @note One probe, not two. @c working_ledger::insert walks the chain from the
 *       id's home slot and stops either at the entry already there or at the
 *       first free slot after it, so asking "is it present" separately would
 *       walk the same cluster twice. @see detail::probe_table::vacancy_for
 *
 * @note @c DUPLICATE_ORDER reports the same @c reject_reason the book would
 *       give, @c DUPLICATE_ORDER_ID: the gate makes the refusal earlier, and a
 *       client should not be able to tell which boundary answered.
 */
[[nodiscard]] RISK_MANAGEMENT_EXPORT breach_bits claim(working_ledger &ledger,
								const engine::orders::order &o) noexcept;

} // namespace exchange::risk::hooks::pre_trade

#pragma once
// What kind of lifecycle record an outcome is.
//
// Split from outcome.hpp so a caller that only switches on the kind - a client
// gateway, a journal reader, a formatter - needs neither the record's layout
// nor the factories that build one. The X-macro list that generates the
// enumerator names is part of the enum and travels with it.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine {

#define OUTCOME_TYPE_LIST(X)                                                   \
	X(ACCEPTED, "the book took the order; it is now LIVE")                     \
	X(REJECTED, "the order never entered the book")                            \
	X(FILL, "quantity executed against this order")                            \
	X(CANCELLED, "the unexecuted remainder was withdrawn")                     \
	X(CANCEL_REJECTED, "a cancel request the book could not apply")             \
	X(MODIFIED, "a resting order's price or quantity was changed")             \
	X(MODIFY_REJECTED, "an amendment the book could not apply")

/**
 * @brief What happened to an order.
 *
 * The transition, not the resulting state - @c order_outcome carries both,
 * because they answer different questions. A FILL leaves the order
 * PARTIALLY_FILLED or FILLED; only the outcome type says an execution is what
 * caused it.
 *
 * CANCEL_REJECTED is separate from REJECTED on purpose: rejecting an *order*
 * means it never entered the book, while declining a *cancel request* leaves an
 * order that is alive and well, or that filled and left. MODIFY_REJECTED is the
 * same distinction for an amendment, and for the same reason.
 *
 * MODIFIED says an amendment was applied and nothing more. What it does *not*
 * say is where the order ended up, because an outcome names quantities and a
 * status and has never carried a price - ACCEPTED does not echo an order's
 * price either. The client asked for the price; the venue confirming that it
 * did as asked is the whole content of the record. What the client could not
 * have known is the quantity, since an amendment races the fills that were
 * already in flight, and that is on the record.
 *
 * @note An amendment that changes price re-crosses the book, so a MODIFIED is
 *       followed by the FILLs it caused, exactly as an ACCEPTED is. And an
 *       amendment down to at or below the executed quantity is a withdrawal
 *       rather than a change, so it reports CANCELLED - there is no MODIFIED
 *       for an order that stopped existing.
 *
 * @warning Enumerators are appended, never inserted. The values are what
 *          @c event::command_type's are not - nothing writes an OutcomeType to
 *          disk today - but the two lists are read side by side often enough
 *          that one rule for both is cheaper than remembering which is which.
 */
enum class OutcomeType : std::uint8_t {
	EXCHANGE_ENUM_VALUES(OUTCOME_TYPE_LIST)
};

EXCHANGE_ENUM_NAME(OutcomeType, to_string, OUTCOME_TYPE_LIST)


#undef OUTCOME_TYPE_LIST
} // namespace exchange::engine

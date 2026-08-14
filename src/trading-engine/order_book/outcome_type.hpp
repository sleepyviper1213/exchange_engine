#pragma once
// What kind of lifecycle record an outcome is.
//
// Split from outcome.hpp so a caller that only switches on the kind — a client
// gateway, a journal reader, a formatter — needs neither the record's layout
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
	X(CANCEL_REJECTED, "a cancel request the book could not apply")

/**
 * @brief What happened to an order.
 *
 * The transition, not the resulting state — @c order_outcome carries both,
 * because they answer different questions. A FILL leaves the order
 * PARTIALLY_FILLED or FILLED; only the outcome type says an execution is what
 * caused it.
 *
 * CANCEL_REJECTED is separate from REJECTED on purpose: rejecting an *order*
 * means it never entered the book, while declining a *cancel request* leaves an
 * order that is alive and well, or that filled and left.
 */
enum class OutcomeType : std::uint8_t {
	EXCHANGE_ENUM_VALUES(OUTCOME_TYPE_LIST)
};

EXCHANGE_ENUM_NAME(OutcomeType, to_string, OUTCOME_TYPE_LIST)


#undef OUTCOME_TYPE_LIST
} // namespace exchange::engine

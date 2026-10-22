#pragma once
#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>

namespace exchange::engine {

#define ALLOCATION_POLICY_LIST(X)                                              \
	X(PRICE_TIME, "price-time priority: the level's FIFO, oldest first")       \
	X(PRO_RATA,                                                                \
	  "pro-rata: a partial sweep splits by resting size, residual by time")

/**
 * @brief How one level divides an aggressor that cannot take all of it.
 *
 * @par Why this is a policy and not a constant
 * Price priority is not negotiable - a better price trades first, everywhere,
 * or the book is not a book. What *is* a venue's choice is the tie-break
 * between orders resting at the *same* price, and the two answers in use reward
 * opposite behaviour:
 *
 * - @c PRICE_TIME rewards being early. The level is a FIFO and the whole queue
 *   in front of an order trades before any of it does, so the marginal lot goes
 *   to whoever arrived first. Equities, most crypto venues, and this engine's
 *   default.
 * - @c PRO_RATA rewards being big. A sweep is split in proportion to resting
 *   size, so a quote's expected fill is its share of the level rather than its
 *   place in a line. The short end of the rates complex (CME's SOFR/Eurodollar
 *   contracts) allocates this way, because a level there holds tens of
 *   thousands of lots and a pure FIFO queue at the front month is unwinnable:
 *   the venue would rather pay size to stand up than pay latency to get in
 *   first.
 *
 * The distinction is worth a type because it changes what a passive strategy is
 * optimising. Under @c PRICE_TIME the question is "how many lots are ahead of
 * me, and will this sweep reach them" - queue position is the alpha. Under
 * @c PRO_RATA there is no queue to be at the front of: every resting order at
 * the price takes part in every trade, and the question is "what fraction of
 * this level am I". @c order_book::projected_fill answers both with the same
 * arithmetic the matcher runs. @see queue_position
 *
 * @par What both policies agree on
 * A sweep that takes the *whole* level. If the aggressor's remaining quantity
 * is at least the level's aggregate, every resting order fills in full and
 * there is nothing to divide - the policies differ only in the order the trades
 * print, and the matcher takes the FIFO path for both because it is the cheaper
 * one. Allocation is only ever a question about a *partial* sweep.
 *
 * @par What PRO_RATA here does not model
 * CME's algorithm has two optional stages this does not: **top-order
 * allocation**, which hands a fixed percentage to the first order at the price
 * before anything is divided, and a **minimum allocation** larger than one lot.
 * Both are per-contract parameters rather than properties of pro-rata, so they
 * belong on @c symbol_spec if they are ever wanted, and neither changes the
 * shape of the code here. What is implemented is the core: proportional shares,
 * rounded down, with the rounding residual settled by time priority.
 */
enum class allocation_policy : std::uint8_t {
	EXCHANGE_ENUM_VALUES(ALLOCATION_POLICY_LIST)
};

/// @brief The enumerator name of an @c allocation_policy, e.g. @c "PRO_RATA".
EXCHANGE_ENUM_NAME(allocation_policy, to_string, ALLOCATION_POLICY_LIST)

/// @brief A short description of what @p policy does to a partial sweep.
EXCHANGE_ENUM_LABEL_ONLY(allocation_policy, describe, ALLOCATION_POLICY_LIST)

#undef ALLOCATION_POLICY_LIST
} // namespace exchange::engine

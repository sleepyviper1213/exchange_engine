#pragma once
// The arithmetic of dividing an aggressor between the orders resting at one
// price. Header-only and `detail` because it has exactly two callers, both in
// order_book.cpp: the matching loop, which divides for real, and
// order_book::projected_fill, which divides hypothetically. They must agree to
// the lot - a projection computed by a second, similar-looking formula would be
// a quoter's model of a venue rather than the venue - so the division lives
// here once and neither caller owns a copy of it.

#include "../allocation_policy.hpp"
#include "../price_level.hpp"
#include "orders/types.hpp"
#include "resting_order.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace exchange::engine::detail {

/**
 * @brief The queue in front of one order: what trades before it, and in how
 *        many separate orders.
 * @see ahead_of
 */
struct queue_ahead {
	volume_t lots      = 0;
	std::size_t orders = 0;
};

/**
 * @brief Walk @p level's FIFO up to @p node and total what precedes it.
 *
 * O(orders ahead), which is the honest cost of the question: a level is a
 * linked list and an order's rank in it is not stored anywhere. Nothing on the
 * matching path asks - the matcher always stands at the head, where the answer
 * is zero - so this is only ever paid by a caller that wants to *report* a
 * queue position.
 *
 * @pre @p node rests at @p level.
 */
[[nodiscard]] inline queue_ahead ahead_of(const price_level &level,
										  const resting_order &node) noexcept {
	queue_ahead ahead;
	for (const resting_order &other : level.orders) {
		if (&other == &node) return ahead;
		ahead.lots += other.qty();
		++ahead.orders;
	}
	assert(false && "node does not rest at this level");
	return ahead;
}

/**
 * @brief One order's proportional share of @p arriving lots, rounded down.
 *
 * @param arriving Lots the aggressor has left when it reaches the level.
 * @param resting This order's unexecuted quantity.
 * @param level_volume Every unexecuted lot at the price, this order included.
 *
 * Rounded *down*, never to nearest: rounding up would let the shares sum past
 * what the aggressor actually brought, and the level would print more volume
 * than traded. What the flooring leaves over is the residual, and time priority
 * is what settles it - @see pro_rata_residual.
 *
 * @note The multiply happens in @c volume_t, and it has to: at scale, @c
 *       arriving and @c level_volume are both level aggregates, and the product
 *       of an aggregate with an order quantity does not fit in 32 bits for any
 *       book worth matching on.
 *
 * @par The bound the callers rely on
 * A partial sweep has @c arriving < @c level_volume, and then the result is at
 * most @c resting - 1: the division loses at least one lot to every order it
 * touches. That is what guarantees every order has room for the leveling lot
 * the residual pass may hand it, so an allocation can never exceed the order it
 * is allocated to.
 */
[[nodiscard]] constexpr quantity_t
pro_rata_share(volume_t arriving, quantity_t resting,
			   volume_t level_volume) noexcept {
	assert(level_volume > 0 && "a level with no volume divides nothing");
	assert(resting > 0 && "a resting order with no quantity is not resting");
	assert(arriving >= 0 && "an aggressor cannot bring negative liquidity");
	if (arriving >= level_volume) return resting; // the whole level fills
	return static_cast<quantity_t>((arriving * static_cast<volume_t>(resting)) /
								   level_volume);
}

/**
 * @brief Lots @p arriving still has left once every order at @p level has taken
 *        its rounded-down share.
 *
 * Strictly less than the number of orders resting here, because each share
 * loses less than one whole lot to the division - which is the fact that makes
 * the leveling pass finite: one lot apiece down the FIFO always exhausts it,
 * and never gives an order a second lot it might not have room for.
 *
 * @pre @p arriving < @c level.total_volume() - the full-sweep case has no
 *      residual to settle, and every caller has already branched on it.
 */
[[nodiscard]] inline volume_t pro_rata_residual(const price_level &level,
												volume_t arriving) noexcept {
	const volume_t level_volume = level.total_volume();
	assert(arriving < level_volume && "residual asked of a full sweep");
	volume_t residual = arriving;
	for (const resting_order &node : level.orders)
		residual -= pro_rata_share(arriving, node.qty(), level_volume);
	assert(residual >= 0 && "shares summed past the aggressor's quantity");
	assert(residual < static_cast<volume_t>(level.order_count()) &&
		   "flooring cannot lose a whole lot per order");
	return residual;
}

/**
 * @brief Exactly what @p node receives when @p arriving lots reach @p level
 *        under @p policy - the matcher's own answer, asked without matching.
 *
 * @pre @p node rests at @p level.
 * @return Lots allocated to @p node; never more than its remaining quantity,
 *         and zero when the sweep does not reach it at all.
 */
[[nodiscard]] inline quantity_t
allocation_for(const price_level &level, const resting_order &node,
			   volume_t arriving, allocation_policy policy) noexcept {
	if (arriving <= 0) return 0;

	// A sweep that takes the whole level fills every order in it, whichever
	// policy is in force. Only a partial sweep is a division problem.
	const volume_t level_volume = level.total_volume();
	if (arriving >= level_volume) return node.qty();

	if (policy == allocation_policy::PRICE_TIME) {
		// The queue in front trades first and nothing about our own size enters
		// into it: we get what is left when the orders ahead are done, or
		// nothing.
		const volume_t reaching_us = arriving - ahead_of(level, node).lots;
		return static_cast<quantity_t>(
			std::clamp<volume_t>(reaching_us, 0, node.qty()));
	}

	// Pro-rata: our share of the level, plus one leveling lot if the residual
	// pass reaches our place in the FIFO. That second term is time priority
	// still doing work - it is worth at most a lot, and on a level of many
	// small orders it is the difference between a share of zero and a fill.
	const quantity_t share = pro_rata_share(arriving, node.qty(), level_volume);
	const auto rank = static_cast<volume_t>(ahead_of(level, node).orders);
	return share + (rank < pro_rata_residual(level, arriving) ? 1 : 0);
}

} // namespace exchange::engine::detail

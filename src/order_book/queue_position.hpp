#pragma once
#include "allocation_policy.hpp"
#include "fwd.hpp"
#include "orders/types.hpp"

#include <cstddef>

namespace exchange::engine {

/**
 * @brief Where one resting order stands among the orders at its price.
 *
 * @par Why the book answers this at all
 * Because "will this order fill" is not a question about the market, it is a
 * question about the queue. Two orders at the same price with the same size can
 * have completely different lifetimes, and the whole difference is how much
 * liquidity sits in front of them: at the front of a level an order earns the
 * spread on every sweep that reaches its price, and thirty lots back it earns
 * nothing at all while wearing the same adverse selection. That is the
 * distinction passive alpha lives on, and until now nothing in the book could
 * be asked about it - @c volume_at_price returns a level's aggregate, which is
 * the same number for the front of the queue and the back of it.
 *
 * @par What the numbers mean, and which one matters
 * Under @c allocation_policy::PRICE_TIME, @c lots_ahead is the answer: it is
 * exactly the volume that must trade at this price before this order receives
 * a single lot, so @c is_at_front means the next print at this price is ours.
 * Under @c allocation_policy::PRO_RATA there is no such threshold - every
 * resting order at the price takes part in every trade - and the number that
 * matters is this order's *share*, @c remaining over @c level_volume. Both are
 * carried, both are meaningful, and @c policy says which one the venue is
 * actually going to use. @see order_book::projected_fill, which collapses the
 * two into the one question a quoter asks.
 *
 * @note A snapshot, not a handle. The queue moves on every message; this
 *       describes it as of the call and is not updated afterwards.
 *
 * @note Anonymous liquidity (@c add_order, id zero) counts towards the volumes
 *       here - it is real depth that trades ahead of or beside us - but has no
 *       id to look one of these up by.
 */
struct queue_position {
	/// @brief The price this order rests at, in ticks.
	price_t price;

	/// @brief The side it rests on.
	side_t side;

	/// @brief Its own unexecuted quantity - what is still queued, not what it
	///        was placed for. A partial fill shrinks this and costs no
	///        priority.
	quantity_t remaining;

	/// @brief Resting lots with strict priority over this order: everything at
	///        this price that arrived earlier. Zero at the front of the queue.
	///
	/// @c volume_t because it is a sum across orders. @see price_level::volume
	volume_t lots_ahead;

	/// @brief Resting lots that joined this price after this order. They matter
	///        under @c PRO_RATA, where they dilute our share, and not at all
	///        under @c PRICE_TIME, where they are behind us by construction.
	volume_t lots_behind;

	/// @brief How many separate orders are ahead of this one. Paired with
	///        @c lots_ahead because the two answer different questions: one
	///        cancel can retire many lots, and many cancels can retire few.
	std::size_t orders_ahead;

	/// @brief The policy the book will divide this level with. Carried on the
	///        snapshot so a caller reading @c lots_ahead can tell whether that
	///        number is a queue or merely a description. @see allocation_policy
	allocation_policy policy;

	/// @brief Every unexecuted lot at this price, ours included.
	[[nodiscard]] constexpr volume_t level_volume() const noexcept {
		return lots_ahead + static_cast<volume_t>(remaining) + lots_behind;
	}

	/// @brief Nothing at this price trades before us. Under @c PRICE_TIME that
	///        is the best position obtainable; under @c PRO_RATA it is a fact
	///        about arrival order with no effect on the next fill beyond the
	///        rounding residual. @see allocation_policy
	[[nodiscard]] constexpr bool is_at_front() const noexcept {
		return lots_ahead == 0;
	}

	bool operator==(const queue_position &) const noexcept = default;
};

} // namespace exchange::engine

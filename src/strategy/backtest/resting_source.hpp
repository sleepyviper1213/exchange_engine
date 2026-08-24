#pragma once
// Where the fill model learns which of our orders are still resting, and at
// what price.
//
// Split out of fill_model.hpp because it is the one thing about that model that
// differs between running over a capture and running against a live feed, and
// the difference is a statement about *knowledge* rather than about plumbing.
// Offline, the harness is single-threaded and can read the venue's own record
// store directly. Live, the matching engine owns that store on another thread
// and the producer knows only what the return path has told it - which is what
// a real strategy knows, and knows late.

#include "orders/types.hpp"

#include <concepts>
#include <optional>

namespace exchange::strategy::backtest {

/**
 * @brief All the fill model needs to know about one resting order of ours.
 *
 * Three fields, and deliberately not a handle to whatever produced them: the
 * model reads a price, a side and a remaining size, decides what the venue must
 * have traded against them, and never writes back. Copying twenty-four bytes
 * out of the source is cheaper than reasoning about how long the source's own
 * storage stays valid across a settle round.
 */
struct resting_quote {
	side_t side;      ///< which side of the book it joined
	price_t price;    ///< limit price, in ticks
	quantity_t lots;  ///< quantity still working
};

/**
 * @brief Anything that can answer "is this order of ours still resting, and
 *        where".
 *
 * @c std::nullopt means the order is not working - filled, cancelled, rejected,
 * or never known. The model treats all four the same way, because for the
 * question it is asking they *are* the same: an order that is not resting
 * cannot be traded against.
 *
 * @note @c noexcept is required rather than encouraged. The model calls this
 *       once per working order per side per settle round; a lookup that could
 *       throw would put an exception path through the middle of the harness's
 *       hot loop, and on the live path it would put one on the producer thread
 *       between a feed frame and its submission.
 */
template <class Source>
concept resting_order_source =
	requires(const Source &source, order_id_t id) {
		{ source.resting(id) } noexcept
			-> std::same_as<std::optional<resting_quote>>;
	};

} // namespace exchange::strategy::backtest

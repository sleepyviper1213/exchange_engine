#pragma once
// One normalised trade print - the tape, not the book.
// @see normalised.hpp for what "normalised" fixes and why.

#include "core/chrono/ingress.hpp" // ingress_time
#include "fwd.hpp"                 // sequence_t
#include "market_data_export.hpp"
#include "normalised.hpp"          // timestamp
#include "orders/types.hpp"        // side_t
#include "types.hpp"               // scaled_price_t, scaled_qty_t

namespace exchange::market_data {

/**
 * @brief One normalised trade print: a match the venue published.
 *
 * The tape, not the book. A print says that a trade happened, at a price, for a
 * size, with one side taking liquidity - and nothing else. It carries no level,
 * no queue position and no order identity, because an aggregate venue feed
 * publishes none of those.
 *
 * It is therefore emphatically *not* a depth message, and it deliberately does
 * not travel in @c feed_message. @see trade_feed.hpp, which states the whole
 * argument for keeping the two feeds apart.
 *
 * @c id is the venue's own trade id. On one symbol's tape it increments by one
 * per print, which makes it a sequence in exactly the sense @c sequence_t
 * names: a consumer detects a dropped print by arithmetic on consecutive ids,
 * the same way @c depth_sequencer detects a dropped diff, rather than by
 * trusting the transport not to lose one.
 */
struct trade_print {
	/// @brief The venue's trade id - one per print, consecutive on one tape.
	sequence_t id = 0;
	timestamp event_time{}; ///< When the trade executed, ns since epoch.
	/// @brief When this process received the frame this was decoded from, or a
	///        default stamp if nothing stamped it. @see
	///        core::chrono::ingress_clock
	/// NOLINTNEXTLINE(readability-redundant-member-init)
	core::chrono::ingress_time ingress{};
	scaled_price_t price = 0; ///< Execution price, scaled by 10^price_decimals.
	scaled_qty_t qty     = 0; ///< Executed size, scaled by 10^qty_decimals.
	/**
	 * @brief The side the aggressing order would have rested on had it not
	 *        traded through - @c ask when the taker was selling.
	 *
	 * Named for the taker's own side rather than for the side it consumed,
	 * and the distinction matters because those two are opposites: a field
	 * named for the wrong one is a sign error that nothing downstream can
	 * catch, since both spellings type-check and both are plausible. A market
	 * sell is an order on the ask side that happens to cross, so it is
	 * @c side_t::ask here, and it consumes bids.
	 *
	 * @see binance::normalise(const trade_message &), which is the one place
	 *      the venue's "was the buyer the maker?" spelling becomes this.
	 */
	side_t aggressor = side_t::bid;
};

} // namespace exchange::market_data

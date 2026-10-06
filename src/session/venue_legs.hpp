#pragma once
// Which venue order currently stands for each engine order.
//
// The engine amends an order in place and keeps its id. Binance spot cannot:
// a reprice is a cancel-replace, and every replacement is a new venue order -
// a *leg* - with its own client id and its own execution count starting from
// zero. Something has to remember the chain, or three things go wrong, each of
// them seen on testnet before this existed:
//
//   * a cancel names a leg the venue already withdrew, and the leg that is
//     actually working stays in the book after the process exits;
//   * a fill on the new leg reports a cumulative quantity that restarted at
//     zero, so the quoter's "traded so far" goes backwards and the next
//     amendment sizes itself off it;
//   * two replaces of one order in flight at once - the second names the leg
//     the first creates, so if the first fails the second names an order that
//     never existed, and the venue's "unknown order" reads as "already gone".
//
// The third is why a replace is *serialised* per order: while one is in flight
// a later amendment or cancel is held here, and released by the answer against
// whichever leg the venue confirmed. @see order_router::on_replace_answered
//
// --- why this is session's and not venue's ---------------------------------
//
// Because the chain is keyed by the engine's order id and fed by the engine's
// commands. `venue/` speaks client ids and decimals and knows neither; the
// router, which already sits where both vocabularies meet, owns one of these.

#include "orders/amendment.hpp"
#include "orders/side.hpp"
#include "orders/time_in_force_instruction.hpp"
#include "orders/types.hpp"
#include "session/client_order_id.hpp"
#include "session_export.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace exchange::session {

/// @brief What became of a cancel-replace, as far as this process can tell.
enum class replace_answer : std::uint8_t {
	replaced,   ///< the new leg is working and the old one is gone
	unchanged,  ///< refused with the old leg still working
	order_gone, ///< the leg being replaced was no longer working
	withdrawn,  ///< the old leg was cancelled and the new one refused
	unanswered, ///< written, never answered; what it did is unknown
};

/// @brief One engine order's chain of venue orders.
struct venue_leg_chain {
	order_id_t id = 0;
	side_t side   = side_t::bid;
	engine::orders::time_in_force_instruction tif =
		engine::orders::time_in_force_instruction::GOOD_TILL_CANCELLED;

	/// @brief The leg the venue last confirmed - or, before any answer, the
	///        one we placed. A cancel names this one.
	venue_leg_t working = 0;

	/// @brief The leg a replace in flight would create, if one is.
	std::optional<venue_leg_t> replacing{};

	/// @brief The number the next replacement will be placed as.
	venue_leg_t next_leg = 1;

	/// @brief The latest amendment that arrived while a replace was in flight.
	///        Only the latest: an amendment states where the order should be,
	///        not how to get there, so an older one is simply superseded.
	std::optional<engine::orders::amendment> held_amendment{};

	/// @brief A cancel arrived while a replace was in flight.
	bool is_cancel_held = false;

	/// @brief Sequence number of the last request queued for this order.
	///        @see outbound_request::sequence
	std::uint64_t last_request = 0;

	/// @brief One leg's executions as the venue last reported them.
	struct leg_fill {
		venue_leg_t leg       = 0;
		quantity_t cumulative = {};
		bool is_done          = false; ///< reported terminal
	};

	/// @brief Executions on legs that have finished, summed.
	quantity_t settled{};

	/// @brief Legs that are live, or finished but not yet folded into
	///        @c settled. Rarely more than two: the working leg, and the one a
	///        replace in flight is withdrawing.
	std::vector<leg_fill> open_legs{};

	/// @brief Whether @p leg is one this chain may still hear about as live.
	[[nodiscard]] bool is_live(venue_leg_t leg) const noexcept {
		return leg == working || (replacing.has_value() && *replacing == leg);
	}

	/// @brief Executed across every leg - the engine order's own traded
	///        quantity, which is what an amendment's size is counted from.
	[[nodiscard]] SESSION_EXPORT quantity_t traded() const noexcept;
};

/**
 * @brief Every engine order this session has at the venue, and its legs.
 *
 * @note Producer thread, like the router that owns it. Allocates per order it
 *       tracks - this is the venue path, which already builds a signed string
 *       per request, not the matching path.
 */
class venue_legs {
public:
	/// @brief Start tracking @p id, placed as leg zero.
	SESSION_EXPORT void open(order_id_t id, side_t side,
							 engine::orders::time_in_force_instruction tif);

	/// @brief The chain for @p id, or null if this session never placed it or
	///        has already seen it finish.
	[[nodiscard]] SESSION_EXPORT venue_leg_chain *find(order_id_t id) noexcept;
	[[nodiscard]] SESSION_EXPORT const venue_leg_chain *
	find(order_id_t id) const noexcept;

	/// @brief The leg a cancel of @p id should name. Zero for an order this
	///        session is not tracking, which is the spelling it was placed
	///        under if it was ever placed at all.
	[[nodiscard]] SESSION_EXPORT venue_leg_t
	working_leg(order_id_t id) const noexcept;

	/**
	 * @brief Book what a report about leg @p leg of @p id says.
	 *
	 * @param cumulative The leg's own cumulative execution, as the venue
	 *        reported it - counted from that leg's placement, not the order's.
	 * @param is_terminal Whether the leg can trade no further.
	 * @return The engine order's lifetime traded quantity afterwards, or
	 *         nothing if @p id is not tracked.
	 *
	 * The maximum of what each leg has reported is kept, not the latest: a
	 * report delivered twice or out of order must not move the total back.
	 * The chain is dropped once its working leg is terminal with no replace in
	 * flight - a terminal report while one *is* in flight is the replace's own
	 * withdrawal of the old leg, or the fill that will make it fail, and the
	 * answer settles which.
	 */
	SESSION_EXPORT std::optional<quantity_t> on_report(order_id_t id,
													   venue_leg_t leg,
													   quantity_t cumulative,
													   bool is_terminal);

	/**
	 * @brief Begin a replace of @p id's working leg.
	 * @return The leg the replacement will be placed as.
	 * @pre @p id is tracked and has no replace in flight.
	 */
	SESSION_EXPORT venue_leg_t begin_replace(order_id_t id) noexcept;

	/**
	 * @brief Apply the venue's answer to @p id's replace in flight.
	 *
	 * @c replaced moves the working leg forward and @c unchanged leaves it;
	 * @c order_gone and @c withdrawn drop the chain, since nothing of the order
	 * is working any more. @c unanswered is read as @c replaced: the request
	 * was written and most likely applied, and if it was not, the exit sweep
	 * is what finds the leg a later cancel then misses.
	 *
	 * Either way a leg that finished before the answer arrived - the old one's
	 * withdrawal, or a replacement that filled at once - is settled here, and
	 * the chain dropped if the leg it now stands on is already finished.
	 */
	SESSION_EXPORT void resolve(order_id_t id, replace_answer answer) noexcept;

	/// @brief Stop tracking @p id.
	SESSION_EXPORT void forget(order_id_t id) noexcept;

	/// @brief Orders tracked right now.
	[[nodiscard]] std::size_t size() const noexcept { return chains_.size(); }

	/// @brief Every chain, for a reconciliation to walk.
	[[nodiscard]] std::span<const venue_leg_chain> chains() const noexcept {
		return chains_;
	}

private:
	std::vector<venue_leg_chain> chains_;
};

} // namespace exchange::session

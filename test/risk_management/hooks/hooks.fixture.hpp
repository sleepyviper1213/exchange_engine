#pragma once
// A desk with two listings, a gate each, and the router that feeds them - the
// smallest wiring in which routing can be wrong.
//
// One position book across both gates on purpose: it is what they genuinely
// share in a deployment, and it is where a misrouted print shows up as a
// position on the listing that never traded.
//
// The two gates' listings and the router's capacity are constructor parameters
// rather than constants because the contract suites need the degenerate shapes -
// two gates on *one* listing, and a gate whose listing is past the table.

#include "../gate/gate.fixture.hpp" // IWYU pragma: export
#include "risk_management/hooks/feedback.hpp"
#include "trading-engine/order_book/order_state.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/reject_reason.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <algorithm>
#include <cstddef>

// A fixture at global scope cannot see these for free. @see testing.md
using exchange::symbol_id_t;
using exchange::volume_t;
using exchange::engine::OrderStatus;
using exchange::engine::OutcomeType;
using exchange::engine::reject_reason;

/// @brief The second listing every hooks suite trades. @c SYMBOL is the first.
inline constexpr symbol_id_t OTHER_SYMBOL = 2;

/// @brief A listing inside the routable range that no gate screens.
inline constexpr symbol_id_t UNSCREENED_SYMBOL = 5;

/// @brief A buy on @p symbol, since the risk fixture's @c buy always stamps
///        @c SYMBOL.
[[nodiscard]] inline order buy_on(symbol_id_t symbol, order_id_t id,
								  price_t price, quantity_t qty) {
	return {.id        = id,
			.symbol_id = symbol,
			.side      = side_t::bid,
			.price     = price,
			.qty       = qty};
}

/// @brief The outcome the book emits when an order is withdrawn unfilled.
[[nodiscard]] inline order_outcome withdrawn(order_id_t id, quantity_t left) {
	return {.id        = id,
			.type      = OutcomeType::CANCELLED,
			.reason    = reject_reason::NONE,
			.status    = OrderStatus::CANCELLED,
			.traded    = 0,
			.remaining = left};
}

/// @brief A print between two ids at @p price. Named for what it is on the
///        return path, and distinct from the two `print` helpers the strategy
///        and event fixtures already own.
[[nodiscard]] inline trade filled_at(order_id_t aggressor, order_id_t resting,
									 price_t price, quantity_t volume) {
	return {.aggressor = aggressor,
			.resting   = resting,
			.price     = price,
			.volume    = volume};
}

/// @brief Two gates over one position book, plus the router between them and a
///        partition's published events.
class feedback_desk {
public:
	/// @brief Listings the router can address by default - a highest-id bound.
	static constexpr symbol_id_t LISTINGS = 8;

	/**
	 * @param second_symbol The listing the second gate screens. Pass @c SYMBOL
	 *        to build the one shape @c attach refuses: two gates, one listing.
	 * @param listings The router's capacity. Pass fewer than @p second_symbol to
	 *        build a gate the router cannot address at all.
	 */
	explicit feedback_desk(symbol_id_t second_symbol = OTHER_SYMBOL,
						   symbol_id_t listings      = LISTINGS)
		: positions_(std::max<std::size_t>(listings, second_symbol + 1U)),
		  second_(sink_, second_symbol, permissive(), positions_, breaker_, 0,
				  clock_),
		  router_(listings) {}

	/// @brief Wire both gates up. Left to the suite rather than done in the
	///        constructor, because half the contract is about what @c attach
	///        refuses.
	void attach_all() {
		router_.attach(first_);
		router_.attach(second_);
	}

	/// @brief Place @p qty on @p symbol's gate, so it has working exposure.
	[[nodiscard]] bool place(symbol_id_t symbol, order_id_t id, price_t price,
							 quantity_t qty) {
		return gate(symbol).submit(
			command::place(buy_on(symbol, id, price, qty)));
	}

	[[nodiscard]] test_gate &gate(symbol_id_t symbol) noexcept {
		return symbol == SYMBOL ? first_ : second_;
	}

	[[nodiscard]] test_gate &first() noexcept { return first_; }

	[[nodiscard]] test_gate &second() noexcept { return second_; }

	[[nodiscard]] exchange::risk::hooks::feedback_router<test_gate> &
	router() noexcept {
		return router_;
	}

	[[nodiscard]] volume_t net(symbol_id_t symbol) const noexcept {
		return positions_.net_lots(symbol);
	}

	[[nodiscard]] volume_t working(symbol_id_t symbol,
								   side_t side) const noexcept {
		return positions_.working_lots(symbol, side);
	}

private:
	recording_sink sink_;
	position_book positions_;
	circuit_breaker breaker_;
	manual_clock clock_;
	test_gate first_{sink_, SYMBOL, permissive(), positions_, breaker_, 0,
					 clock_};
	test_gate second_;
	exchange::risk::hooks::feedback_router<test_gate> router_;
};

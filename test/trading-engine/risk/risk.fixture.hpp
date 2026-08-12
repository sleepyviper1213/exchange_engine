#pragma once
// Shared scaffolding for the risk suites: a clock a test can move by hand, and
// the small builders that keep a command out of the assertion.

#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/risk/clock.hpp"
#include "trading-engine/risk/limits.hpp"

// The strategy tree already has the sink these suites need — one that records
// what it is given and can be told to refuse, which is exactly how a full SPSC
// queue looks from the producer side. Reaching for it beats copying it: a gate
// *is* a command_sink, so the two components are testing against the same
// interface and should be testing against the same double.
#include "../strategy/strategy.fixture.hpp" // IWYU pragma: export

#include <cstdint>
#include <memory>



// Only what this header's own declarations name; a suite reaches the rest with
// using-directives on the engine namespaces. The scalars have to be spelled out
// because these fixtures sit at global scope — nothing here is nested inside
// `exchange`, so nothing is inherited from it.
using exchange::order_id_t;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::symbol_id_t;

using exchange::engine::orders::order;
using exchange::engine::risk::risk_limits;

/// @brief The listing every risk suite trades.
inline constexpr symbol_id_t SYMBOL = 1;

/**
 * @brief A clock a test sets rather than waits for.
 *
 * The state is behind a @c shared_ptr because the gate takes its clock *by
 * value* — it is usually a stateless functor and holding it inline is the
 * point. A test still needs to move time after the gate has been built, so the
 * handle is copied and the reading is shared.
 */
class manual_clock {
public:
	[[nodiscard]] std::uint64_t now_ns() const noexcept { return *now_; }

	void set(std::uint64_t t) noexcept { *now_ = t; }

	void advance(std::uint64_t delta) noexcept { *now_ += delta; }

private:
	std::shared_ptr<std::uint64_t> now_ = std::make_shared<std::uint64_t>(0);
};

static_assert(exchange::engine::risk::nanosecond_clock<manual_clock>);

/// @brief A plain limit order on @c SYMBOL, ready to be placed.
[[nodiscard]] inline order buy(order_id_t id, price_t price, quantity_t qty) {
	return {.id        = id,
			.symbol_id = SYMBOL,
			.side      = side_t::bid,
			.price     = price,
			.qty       = qty};
}

/// @brief The sell-side counterpart of @c buy.
[[nodiscard]] inline order sell(order_id_t id, price_t price, quantity_t qty) {
	return {.id        = id,
			.symbol_id = SYMBOL,
			.side      = side_t::ask,
			.price     = price,
			.qty       = qty};
}

/**
 * @brief Limits that refuse nothing, as a base for a test to tighten one field
 *        of.
 *
 * Every screening test is about exactly one rule, and starting from "everything
 * is allowed" is what keeps it that way — a test that tripped two limits at
 * once would pass for the wrong reason the day the severity order changed.
 */
[[nodiscard]] inline risk_limits permissive() { return risk_limits{}; }


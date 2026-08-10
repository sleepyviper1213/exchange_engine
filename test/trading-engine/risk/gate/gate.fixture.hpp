#pragma once
// One gate, one sink, one clock, wired the way an app would wire them — so a
// suite says what it is testing and not how a gate is built.

#include "../risk.fixture.hpp" // IWYU pragma: export

#include "trading-engine/event/command.hpp"
#include "trading-engine/order_book/outcome.hpp"
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"
#include "trading-engine/risk/circuit_breaker.hpp"
#include "trading-engine/risk/gate.hpp"
#include "trading-engine/risk/position.hpp"

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace exchange::test::risk {

using exchange::engine::order_outcome;
using exchange::engine::trade;
using exchange::engine::event::command;
using exchange::engine::risk::breach;
using exchange::engine::risk::circuit_breaker;
using exchange::engine::risk::position_book;
using exchange::engine::risk::trading_state;

/// @brief A window small enough that a test can step across it in a literal.
inline constexpr unsigned TEST_WINDOW_LOG2 = 10;
inline constexpr std::uint64_t TEST_WINDOW_NS = std::uint64_t{1}
												<< TEST_WINDOW_LOG2;

/// @brief The gate under test, over a sink that records and a clock a test
///        moves.
using test_gate =
	exchange::engine::risk::risk_gate<recording_sink, manual_clock>;

/**
 * @brief A gate and everything it needs, assembled.
 *
 * Deliberately not a @c ::testing::Test: the suites differ in the limits they
 * want and a fixture that took them through a member would have to be
 * constructed twice. A plain aggregate built in the test body reads better and
 * lets one test hold two gates when it needs to.
 */
class harness {
public:
	explicit harness(const risk_limits &limits = permissive(),
					 price_t reference         = 0,
					 std::uint32_t auto_trip = circuit_breaker::NO_AUTO_TRIP)
		: breaker_(auto_trip, TEST_WINDOW_LOG2),
		  gate_(sink_, SYMBOL, limits, positions_, breaker_, reference, clock_) {
	}

	// --- driving ----------------------------------------------------------

	/// @brief Submit one order as a PLACE.
	[[nodiscard]] bool place(const order &o) { return gate_.submit(command::place(o)); }

	/// @brief Submit one CANCEL.
	[[nodiscard]] bool cancel(order_id_t id) {
		return gate_.submit(command::cancel(SYMBOL, id));
	}

	/// @brief Submit a whole batch, so intra-batch accumulation is exercised.
	[[nodiscard]] bool submit(std::initializer_list<command> batch) {
		const std::vector<command> owned{batch};
		return gate_.submit_range(owned);
	}

	/// @brief Report an execution back, as the event loop would.
	void filled(order_id_t aggressor, order_id_t resting, price_t price,
				quantity_t volume) {
		gate_.on_trade(trade{.aggressor = aggressor,
							 .resting   = resting,
							 .price     = price,
							 .volume    = volume});
	}

	/// @brief Report a lifecycle record back, as the event loop would.
	void outcome(const order_outcome &record) { gate_.on_outcome(record); }

	// --- asking -----------------------------------------------------------

	[[nodiscard]] test_gate &gate() noexcept { return gate_; }
	[[nodiscard]] recording_sink &sink() noexcept { return sink_; }
	[[nodiscard]] position_book &positions() noexcept { return positions_; }
	[[nodiscard]] circuit_breaker &breaker() noexcept { return breaker_; }
	[[nodiscard]] manual_clock &clock() noexcept { return clock_; }

	/// @brief Commands the sink actually received.
	[[nodiscard]] const std::vector<command> &delivered() const noexcept {
		return sink_.commands();
	}

	/// @brief The single rejection the last submit produced.
	/// @pre exactly one command was refused.
	[[nodiscard]] const order_outcome &sole_rejection() const {
		return gate_.rejections().front();
	}

	/// @brief Whether the gate counted @p rule at least once.
	[[nodiscard]] bool saw(breach rule) const noexcept {
		return gate_.breaches(rule) > 0;
	}

	[[nodiscard]] volume_t net() const noexcept {
		return positions_.net_lots(SYMBOL);
	}

	[[nodiscard]] volume_t working(side_t side) const noexcept {
		return positions_.working_lots(SYMBOL, side);
	}

private:
	// Declaration order is load-bearing: the gate takes references to the three
	// above it and asserts on the position book at construction.
	recording_sink sink_;
	position_book positions_{8};
	circuit_breaker breaker_;
	manual_clock clock_;
	test_gate gate_;
};

} // namespace exchange::test::risk

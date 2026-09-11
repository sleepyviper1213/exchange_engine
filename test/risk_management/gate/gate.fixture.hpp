#pragma once
// One gate, one sink, one clock, wired the way an app would wire them - so a
// suite says what it is testing and not how a gate is built.

#include "../risk.fixture.hpp" // IWYU pragma: export
#include "event/command.hpp"
#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"
#include "orders/types.hpp"
#include "risk_management/gate.hpp"
#include "risk_management/hooks/pre_trade/position.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"

#include <cstdint>
#include <span>
#include <vector>


// Only what the declarations below name. @see risk.fixture.hpp
// The scalar vocabulary. Spelled out because these fixtures sit at global
// scope: nothing here is inside `exchange`, so nothing is inherited from it.
using exchange::order_id_t;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::volume_t;

using exchange::engine::order_outcome;
using exchange::engine::trade;
using exchange::engine::event::command;
using exchange::risk::hooks::breach;
using exchange::risk::hooks::pre_trade::position_book;
using exchange::risk::hooks::system::circuit_breaker;

/// @brief A window small enough that a test can step across it in a literal.
inline constexpr unsigned TEST_WINDOW_LOG2    = 10;
inline constexpr std::uint64_t TEST_WINDOW_NS = 1ull << TEST_WINDOW_LOG2;

/// @brief The gate under test, over a sink that records and a clock a test
///        moves.
using test_gate = exchange::risk::risk_gate<recording_sink, manual_clock>;

/**
 * @brief Breaches within one window that trip the breaker.
 *
 * A named type rather than a bare @c std::uint32_t because the parameter next
 * to it is a @c price_t, and both are 32-bit unsigned - so @c harness{limits,
 * 3, 100} would compile with the two transposed and quietly configure a breaker
 * that never trips against a mark of 3. clang-tidy's
 * easily-swappable-parameters check flags exactly that shape. Naming it makes
 * the transposition a compile error instead.
 */
struct auto_trip_after {
	std::uint32_t breaches = circuit_breaker::NO_AUTO_TRIP;
};

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
					 price_t reference = 0, auto_trip_after trip = {})
		: breaker_(trip.breaches, TEST_WINDOW_LOG2),
		  gate_(sink_, SYMBOL, limits, positions_, breaker_, reference,
				clock_) {}

	// --- driving ----------------------------------------------------------

	/// @brief Submit one order as a PLACE.
	[[nodiscard]] bool place(const order &o) {
		return gate_.submit(command::place(o));
	}

	/// @brief Submit one CANCEL.
	[[nodiscard]] bool cancel(order_id_t id) {
		return gate_.submit(command::cancel(SYMBOL, id));
	}

	/// @brief Submit a whole batch, so intra-batch accumulation is exercised.
	[[nodiscard]] bool submit(std::span<const command> batch) {
		return gate_.submit_range(batch);
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

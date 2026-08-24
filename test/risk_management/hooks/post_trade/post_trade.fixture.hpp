#pragma once
// What the post-trade suites are measured against: a breaker nobody else
// trips, a monitor over one listing, and the four outcome shapes the ratio's
// counting policy turns on.
//
// The breaker is default-constructed on purpose. `circuit_breaker`'s own
// auto-trip counts *breaches*, and a suite here wants the only thing that can
// open the switch to be the rule under test - otherwise a trip is evidence of
// nothing in particular. @see auto_trip_after for the suites that do want it.
//
// Trades, and the outcome shapes that already had names, come from the fixtures
// above: `print` for a tape print, `filled` for a fill, `withdrawn` for a
// client cancel. Only the shapes no earlier suite needed are added here.

#include "../hooks.fixture.hpp" // IWYU pragma: export
#include "risk_management/hooks/post_trade.hpp"
#include "order_book/order_state.hpp"
#include "order_book/outcome.hpp"
#include "order_book/reject_reason.hpp"
#include "orders/types.hpp"

#include <cstddef>
#include <cstdint>

// A fixture at global scope cannot see these for free, and each is spelled from
// the lane that owns it - which is the point of there being no flat alias.
// @see testing.md
using exchange::risk::hooks::post_trade::fill_burst;
using exchange::risk::hooks::post_trade::order_trade_ratio;
using exchange::risk::hooks::post_trade::outcome_silence;
using exchange::risk::hooks::post_trade::post_trade_limits;
using exchange::risk::hooks::post_trade::post_trade_monitor;
using exchange::risk::hooks::post_trade::tape_direction;
using exchange::risk::hooks::system::trading_state;
using exchange::risk::hooks::system::trip_cause;

/// @brief A window short enough for a suite to step over by hand. About 1 us.
inline constexpr unsigned POST_TRADE_WINDOW_LOG2    = 10;
inline constexpr std::uint64_t POST_TRADE_WINDOW_NS = std::uint64_t{1}
													  << POST_TRADE_WINDOW_LOG2;

/// @brief Silence a suite can wait out without waiting. About 1 ms.
inline constexpr std::uint64_t POST_TRADE_TIMEOUT_NS = 1'000'000;

/// @brief Thresholds with every rule off - what a deployment that has not
///        configured surveillance gets, and the base every suite tightens one
///        field of.
[[nodiscard]] inline post_trade_limits surveillance() {
	post_trade_limits limits{};
	limits.ratio_window_log2_ns = POST_TRADE_WINDOW_LOG2;
	limits.burst_window_log2_ns = POST_TRADE_WINDOW_LOG2;
	return limits;
}

/// @brief Thresholds with only the ratio rule armed.
[[nodiscard]] inline post_trade_limits surveillance_ratio(std::uint32_t cap,
														  std::uint32_t floor) {
	post_trade_limits limits          = surveillance();
	limits.max_messages_per_execution = cap;
	limits.min_messages_to_judge      = floor;
	return limits;
}

/// @brief Thresholds with only the burst and run rules armed.
[[nodiscard]] inline post_trade_limits
surveillance_burst(std::uint32_t max_executions, exchange::volume_t max_volume,
				   std::uint32_t max_run) {
	post_trade_limits limits         = surveillance();
	limits.max_executions_per_window = max_executions;
	limits.max_volume_per_window     = max_volume;
	limits.max_adverse_run           = max_run;
	return limits;
}

/// @brief Thresholds with only the silence rule armed.
[[nodiscard]] inline post_trade_limits
surveillance_silence(std::uint64_t timeout_ns) {
	post_trade_limits limits  = surveillance();
	limits.outcome_timeout_ns = timeout_ns;
	return limits;
}

/// @brief The book took @p id. One message, in the ratio's terms.
[[nodiscard]] inline order_outcome post_trade_ack(order_id_t id,
												  quantity_t qty = 10) {
	return order_outcome::accepted(id, qty);
}

/// @brief @p id never entered the book. Still a message - the venue parsed it
///        and answered.
[[nodiscard]] inline order_outcome post_trade_reject(order_id_t id,
													 quantity_t qty = 10) {
	return order_outcome::rejected(
		id,
		exchange::engine::reject_reason::PRICE_OUTSIDE_COLLAR,
		qty);
}

/// @brief The book withdrawing an IOC remainder on its own initiative. Nobody
///        sent a cancel, so this is the one CANCELLED that is not a message.
[[nodiscard]] inline order_outcome post_trade_ioc_drop(order_id_t id,
													   quantity_t left = 10) {
	exchange::engine::order_state state{left};
	return order_outcome::cancelled(
		id,
		state,
		exchange::engine::reject_reason::TIME_IN_FORCE);
}

/// @brief A cancel the book could not apply. A message: it was received.
[[nodiscard]] inline order_outcome post_trade_cancel_reject(order_id_t id) {
	return order_outcome::cancel_rejected(
		id,
		exchange::engine::reject_reason::UNKNOWN_ORDER);
}

/**
 * @brief A monitor over @c SYMBOL and the breaker it is the only writer of.
 *
 * Holds both because a post-trade rule's whole output is a state change on the
 * breaker: a suite asserts on @c state() and @c cause() far more often than on
 * anything the monitor itself returns.
 */
class post_trade_watch {
public:
	explicit post_trade_watch(const post_trade_limits &limits,
							  std::uint64_t now_ns = 0)
		: monitor_(breaker_, SYMBOL, limits, now_ns) {}

	[[nodiscard]] circuit_breaker &breaker() noexcept { return breaker_; }

	[[nodiscard]] post_trade_monitor &monitor() noexcept { return monitor_; }

	/// @brief What the breaker is letting through.
	[[nodiscard]] trading_state state() const noexcept {
		return breaker_.state();
	}

	/// @brief Why it last stopped.
	[[nodiscard]] trip_cause cause() const noexcept { return breaker_.cause(); }

	/// @brief Whether anything has opened the switch.
	[[nodiscard]] bool is_open() const noexcept {
		return breaker_.state() != trading_state::NORMAL;
	}

private:
	circuit_breaker breaker_;
	post_trade_monitor monitor_;
};

/// @brief The router the post-trade suites wire, on a clock they can move.
///        `steady_nanos` would make every silence assertion a sleep.
using post_trade_router =
	exchange::risk::hooks::feedback_router<test_gate, manual_clock>;

/**
 * @brief One watched listing, one unwatched one, and the router between them
 *        and a partition's published events.
 *
 * The second listing is what proves a monitor is fed its own events and only
 * its own: it has a gate and deliberately no monitor, so a print on it must
 * move the desk's position and leave every surveillance counter alone.
 *
 * The gate and the monitor share the breaker, which is the whole point of the
 * lane - a post-trade trip has to be visible to the pre-trade screen. So the
 * suites can assert the loop closes all the way round: feed the monitor, poll
 * it, and watch the gate start refusing new liquidity.
 */
class post_trade_desk {
public:
	/// @brief Listings the router can address.
	static constexpr symbol_id_t LISTINGS = 8;

	explicit post_trade_desk(const post_trade_limits &limits,
							 std::uint64_t now_ns = 0)
		: watched_(breaker_, SYMBOL, limits, now_ns),
		  router_(LISTINGS, clock_) {
		clock_.set(now_ns);
		router_.attach(gate_, watched_);
		router_.attach(other_);
	}

	/// @brief Place @p qty on @p symbol's gate, so it has working exposure the
	///        silence rule can be about.
	[[nodiscard]] bool place(symbol_id_t symbol, order_id_t id, price_t price,
							 quantity_t qty) {
		return gate(symbol).submit(
			command::place(buy_on(symbol, id, price, qty)));
	}

	[[nodiscard]] test_gate &gate(symbol_id_t symbol) noexcept {
		return symbol == SYMBOL ? gate_ : other_;
	}

	[[nodiscard]] post_trade_monitor &monitor() noexcept { return watched_; }

	[[nodiscard]] post_trade_router &router() noexcept { return router_; }

	[[nodiscard]] circuit_breaker &breaker() noexcept { return breaker_; }

	[[nodiscard]] manual_clock &clock() noexcept { return clock_; }

	[[nodiscard]] trading_state state() const noexcept {
		return breaker_.state();
	}

	[[nodiscard]] trip_cause cause() const noexcept { return breaker_.cause(); }

private:
	recording_sink sink_;
	position_book positions_{LISTINGS};
	circuit_breaker breaker_;
	manual_clock clock_;
	test_gate gate_{
		sink_, SYMBOL, permissive(), positions_, breaker_, 0, clock_};
	test_gate other_{
		sink_, OTHER_SYMBOL, permissive(), positions_, breaker_, 0, clock_};
	post_trade_monitor watched_;
	post_trade_router router_;
};

#pragma once
// Shared scaffolding for the risk benchmarks: a sink that costs nothing, a
// clock that costs nothing, and the limits every family screens against.
//
// Two families need these - gate.bench.cpp for throughput and means,
// latency.bench.cpp for percentiles - and copying a fixture between sibling
// benchmarks is not an option.

#include "risk_management/clock.hpp"
#include "risk_management/limits.hpp"
#include "trading-engine/event/command.hpp"
#include "trading-engine/orders/order.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace exchange::bench::risk {

using exchange::engine::event::command;
using exchange::engine::orders::order;
using exchange::risk::risk_limits;

inline constexpr symbol_id_t SYMBOL = 1;
inline constexpr price_t MARK       = 10000;

/// @brief A sink that accepts and forgets. The gate is what is being measured,
///        so whatever is downstream of it must not appear in the number.
struct null_sink {
	std::size_t count = 0;

	bool submit_range(std::span<const command> batch) noexcept {
		count += batch.size();
		return true;
	}
};

/**
 * @brief A clock that costs nothing, for the benchmarks that want the check
 *        rather than the clock.
 *
 * @c steady_clock::now() is a @c QueryPerformanceCounter on Windows and lands
 * around 20–30 ns - several times the whole per-command budget. The gate reads
 * it once per batch, so in production it amortises to nothing; leaving it in a
 * per-command microbenchmark would measure the clock. Families that want the
 * amortisation to be visible rather than assumed use the real one instead.
 */
/**
 * @brief A monotonic reading at @p ns, for suites that name times as numbers.
 *
 * The rules under test are about window boundaries, so a test wants to say
 * "one nanosecond before the edge" and not build a @c time_point to do it. This
 * is the one place the conversion lives, and spelling it at each call site is
 * the point: the integer is visibly being read as an instant.
 */
[[nodiscard]] inline exchange::risk::monotonic_time at_ns(std::uint64_t ns) {
	return exchange::risk::monotonic_time{
		exchange::risk::monotonic_clock::duration{
			static_cast<exchange::risk::monotonic_clock::rep>(ns)}};
}

struct free_clock {
	std::uint64_t ns = 0;

	[[nodiscard]] exchange::risk::monotonic_time now() const noexcept {
		return exchange::risk::monotonic_time{
			exchange::risk::monotonic_clock::duration{
				static_cast<exchange::risk::monotonic_clock::rep>(ns)}};
	}

	[[nodiscard]] std::uint64_t now_ns() const noexcept { return ns; }
};

/**
 * @brief Limits with every rule armed at a level nothing here breaches.
 *
 * A rule left at "unlimited" would still be evaluated - the checks do not
 * short-circuit - but arming them keeps the comparands realistic and keeps the
 * benchmark honest about measuring all ten rules.
 */
[[nodiscard]] inline risk_limits armed() {
	return risk_limits{.max_order_qty         = 10000,
					   .max_order_notional    = 1'000'000'000,
					   .max_position_lots     = 1'000'000,
					   .max_exposure_notional = 100'000'000'000LL,
					   .max_working_orders    = 1U << 16U,
					   .price_band_bps        = 500,
					   .max_messages_per_window =
						   std::numeric_limits<std::uint32_t>::max()};
}

/// @brief A limit order on @c SYMBOL, alternating side by id parity so a long
///        run oscillates about a flat position instead of walking into the
///        position limit.
[[nodiscard]] inline order limit_order(order_id_t id, quantity_t qty) {
	return {.id        = id,
			.symbol_id = SYMBOL,
			.side      = (id & 1U) != 0 ? side_t::bid : side_t::ask,
			.price     = MARK + static_cast<price_t>(id % 16U),
			.qty       = qty};
}

} // namespace exchange::bench::risk

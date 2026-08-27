#include "fill_burst.hpp"

#include <cassert>

namespace exchange::risk::hooks::post_trade {
namespace {

/**
 * @brief Which way @p price moved from @p previous. @c UNKNOWN for a flat
 *        print, which is the honest answer: nothing moved.
 *
 * Not in the header, and not only to keep it out of the interface: the two
 * parameters are both @c price_t, so a caller can transpose them and silently
 * invert every direction this rule reports. One call site, in this file, is the
 * whole mitigation - and the behaviour is checked through @c record and
 * @c direction, which is where a suite should be looking anyway.
 */
[[nodiscard]] constexpr tape_direction moved(price_t previous,
											 price_t price) noexcept {
	if (price > previous) return tape_direction::UP;
	if (price < previous) return tape_direction::DOWN;
	return tape_direction::UNKNOWN;
}

} // namespace

fill_burst::fill_burst(system::circuit_breaker &breaker,
					   const post_trade_limits &limits) noexcept
	: breaker_(&breaker),
	  max_executions_(limits.max_executions_per_window),
	  max_volume_(limits.max_volume_per_window > 0
					  ? static_cast<std::uint64_t>(limits.max_volume_per_window)
					  : 0),
	  max_run_(limits.max_adverse_run),
	  executions_(limits.burst_window_log2_ns),
	  volume_(limits.burst_window_log2_ns) {}

std::uint32_t fill_burst::extend_run(price_t price) noexcept {
	// The first print establishes a price and nothing else. Leaving the
	// direction UNKNOWN here is what keeps a run from being credited to a move
	// that never happened - there is no previous price for it to be relative
	// to.
	if (last_price_ == 0) {
		last_price_ = price;
		return run_;
	}

	const tape_direction step = moved(last_price_, price);
	last_price_               = price;

	// A flat print is not a move: it neither extends the run nor breaks it.
	if (step == tape_direction::UNKNOWN) return run_;

	if (step == direction_) {
		++run_;
	} else {
		direction_ = step;
		run_       = 1;
	}
	return run_;
}

bool fill_burst::record(core::chrono::monotonic_time now,
						const engine::trade &execution) noexcept {
	assert(execution.volume > 0 &&
		   "the book does not publish a print of nothing");

	const auto lots            = static_cast<std::uint64_t>(execution.volume);
	const std::uint64_t prints = executions_.add(now, 1);
	const std::uint64_t traded = volume_.add(now, lots);
	const std::uint32_t run    = extend_run(execution.price);
	++total_execs_;
	total_lots_ += lots;

	// A breaker somebody has already opened is left alone, so one episode
	// produces one trip and one cause rather than one per print. Recovering
	// does not re-arm it either: the tape turning round means the market moved,
	// not that anybody decided to keep trading.
	if (!breaker_->passes_new_orders()) return false;

	if (is_over_burst(prints, traded, max_executions_, max_volume_)) {
		breaker_->trip(system::trading_state::CANCEL_ONLY,
					   system::trip_cause::FILL_BURST);
		++trips_;
		return true;
	}

	if (is_over_run(run, max_run_)) {
		breaker_->trip(system::trading_state::CANCEL_ONLY,
					   system::trip_cause::ADVERSE_RUN);
		++trips_;
		return true;
	}

	return false;
}

bool fill_burst::is_bursting(core::chrono::monotonic_time now) const noexcept {
	return is_over_burst(executions_.count(now),
						 volume_.count(now),
						 max_executions_,
						 max_volume_);
}

bool fill_burst::is_running() const noexcept {
	return is_over_run(run_, max_run_);
}

std::uint64_t
fill_burst::executions(core::chrono::monotonic_time now) const noexcept {
	return executions_.count(now);
}

std::uint64_t
fill_burst::volume(core::chrono::monotonic_time now) const noexcept {
	return volume_.count(now);
}

std::uint32_t fill_burst::run() const noexcept { return run_; }

tape_direction fill_burst::direction() const noexcept { return direction_; }

price_t fill_burst::last_price() const noexcept { return last_price_; }

std::uint64_t fill_burst::total_executions() const noexcept {
	return total_execs_;
}

std::uint64_t fill_burst::total_volume() const noexcept { return total_lots_; }

std::uint64_t fill_burst::window_ns() const noexcept {
	return executions_.width_ns();
}

std::uint64_t fill_burst::trips() const noexcept { return trips_; }

} // namespace exchange::risk::hooks::post_trade

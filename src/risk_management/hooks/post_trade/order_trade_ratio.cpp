#include "order_trade_ratio.hpp"

namespace exchange::risk::hooks::post_trade {

order_trade_ratio::order_trade_ratio(system::circuit_breaker &breaker,
									 const post_trade_limits &limits) noexcept
	: breaker_(&breaker),
	  threshold_(limits.max_messages_per_execution),
	  floor_(limits.min_messages_to_judge),
	  messages_(limits.ratio_window_log2_ns),
	  executions_(limits.ratio_window_log2_ns) {}

bool order_trade_ratio::record_message(
	core::chrono::monotonic_time now) noexcept {
	const std::uint64_t seen = messages_.add(now, 1);
	++total_messages_;

	// Ruled out before the counts are compared, in this order: a rule nobody
	// configured, and a breaker somebody has already opened. The second is what
	// makes one runaway produce one trip rather than one per message.
	if (threshold_ == NO_LIMIT) return false;
	if (!breaker_->passes_new_orders()) return false;
	if (!is_over_ratio(seen, executions_.count(now), threshold_, floor_))
		return false;

	breaker_->trip(system::trading_state::CANCEL_ONLY,
				   system::trip_cause::ORDER_TRADE_RATIO);
	++trips_;
	return true;
}

void order_trade_ratio::record_execution(
	core::chrono::monotonic_time now) noexcept {
	executions_.add(now, 1);
	++total_executions_;
}

bool order_trade_ratio::is_breaching(
	core::chrono::monotonic_time now) const noexcept {
	return is_over_ratio(messages_.count(now),
						 executions_.count(now),
						 threshold_,
						 floor_);
}

std::uint64_t
order_trade_ratio::messages(core::chrono::monotonic_time now) const noexcept {
	return messages_.count(now);
}

std::uint64_t
order_trade_ratio::executions(core::chrono::monotonic_time now) const noexcept {
	return executions_.count(now);
}

std::uint64_t order_trade_ratio::total_messages() const noexcept {
	return total_messages_;
}

std::uint64_t order_trade_ratio::total_executions() const noexcept {
	return total_executions_;
}

std::uint32_t order_trade_ratio::threshold() const noexcept {
	return threshold_;
}

std::uint64_t order_trade_ratio::window_ns() const noexcept {
	return messages_.width_ns();
}

std::uint64_t order_trade_ratio::trips() const noexcept { return trips_; }

} // namespace exchange::risk::hooks::post_trade

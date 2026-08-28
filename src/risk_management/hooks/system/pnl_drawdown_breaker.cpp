#include "pnl_drawdown_breaker.hpp"

#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management/limits.hpp"

namespace exchange::risk::hooks::system {
bool through_floor(std::int64_t pnl, const risk_limits &limits) noexcept {
	return limits.has_loss_limit() && pnl < -limits.max_loss;
}

bool trip_on_drawdown(
	circuit_breaker &breaker, const risk_limits &limits,
	core::util::function_ref<int64_t() const noexcept> pnl_now) noexcept {
	if (!limits.has_loss_limit()) return false;
	if (!breaker.passes_new_orders()) return false;
	if (!through_floor(static_cast<std::int64_t>(pnl_now()), limits))
		return false;
	breaker.trip(trading_state::CANCEL_ONLY, trip_cause::LOSS_LIMIT);
	return true;
}

} // namespace exchange::risk::hooks::system
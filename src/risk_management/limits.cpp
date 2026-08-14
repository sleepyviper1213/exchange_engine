#include "limits.hpp"

namespace exchange::risk {

[[nodiscard]] bool risk_limits::has_loss_limit() const noexcept {
	return max_loss > NO_LOSS_LIMIT;
}

[[nodiscard]] bool risk_limits::has_price_band() const noexcept {
	return price_band_bps > 0;
}
} // namespace exchange::risk

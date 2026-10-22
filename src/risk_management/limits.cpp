#include "limits.hpp"

namespace exchange::risk {

[[nodiscard]] bool risk_limits::has_loss_limit() const noexcept {
	return max_loss > NO_LOSS_LIMIT;
}

} // namespace exchange::risk

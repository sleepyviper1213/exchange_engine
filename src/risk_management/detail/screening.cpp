#include "screening.hpp"

#include "../limits.hpp"

namespace exchange::risk::detail {

[[nodiscard]] price_band
price_band::around(price_t mark, std::int64_t half_width_bps) noexcept {
	if (half_width_bps <= 0 || mark == 0) return {};

	const auto centre = static_cast<std::int64_t>(mark);
	const std::int64_t half =
		centre * half_width_bps / risk_limits::BPS_DENOMINATOR;
	// A price of zero ticks is never admissible, so the floor is one tick
	// rather than zero even for a band wider than the mark.
	const std::int64_t floor   = centre - half > 1 ? centre - half : 1;
	const std::int64_t ceiling = centre + half;
	return {.low  = static_cast<price_t>(floor),
			.span = static_cast<price_t>(ceiling - floor)};
}

[[nodiscard]] price_t price_band::high() const noexcept { return low + span; }

[[nodiscard]] bool price_band::admits(price_t price) const noexcept {
	return static_cast<price_t>(price - low) <= span;
}

} // namespace exchange::risk::detail
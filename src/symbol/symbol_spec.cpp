#include "symbol_spec.hpp"

#include <cassert>
#include <limits>

namespace exchange::engine {
namespace {

/// @brief 10^n for n in [0, 18], the range int64 can hold.
constexpr std::int64_t pow10(int n) noexcept {
	std::int64_t v = 1;
	for (int i = 0; i < n; ++i) v *= 10;
	return v;
}

/// @brief Ceiling division for positive operands. Needed because the band's
///        low edge is a fraction that must round *up*: plain integer division
///        truncates toward zero, which for the low edge is outward, and no
///        later tick rounding can recover the lost fraction. `10.03 * 0.8` is
///        8.024, which must become 8.03 and not 8.02.
constexpr std::int64_t ceil_div(std::int64_t a, std::int64_t b) noexcept {
	return (a + b - 1) / b;
}

/// @brief Round @p scaled up to the next tick boundary - the low edge of a
///        band moves inward, so the band never admits a price the venue's
///        stated percentage does not cover.
constexpr std::int64_t ceil_to_tick(std::int64_t scaled,
									std::int64_t tick) noexcept {
	return ceil_div(scaled, tick) * tick;
}

/// @brief Round @p scaled down to a tick boundary - the high edge, inward.
constexpr std::int64_t floor_to_tick(std::int64_t scaled,
									 std::int64_t tick) noexcept {
	return (scaled / tick) * tick;
}

} // namespace

symbol_spec::symbol_spec(symbol_id_t id, std::string_view symbol,
						 int price_scale, int qty_scale,
						 std::int64_t tick_scaled, std::int64_t lot_scaled,
						 std::int64_t reference_scaled, std::int64_t collar_bps)
	: id_(id),
	  symbol_(symbol),
	  price_scale_(price_scale),
	  qty_scale_(qty_scale),
	  tick_scaled_(tick_scaled),
	  lot_scaled_(lot_scaled),
	  collar_bps_(collar_bps) {
	// Reference data comes from an operator, not a client: a bad listing is a
	// deployment fault and should stop the process, not reject orders quietly
	// for a whole session.
	assert(price_scale >= 0 && price_scale <= 18 && "price scale out of range");
	assert(qty_scale >= 0 && qty_scale <= 18 && "quantity scale out of range");
	assert(tick_scaled > 0 && "tick size must be positive");
	assert(lot_scaled > 0 && "lot size must be positive");
	assert(reference_scaled > 0 && "reference price must be positive");
	assert(reference_scaled % tick_scaled == 0 &&
		   "reference price is not on the tick grid");
	assert(collar_bps >= 0 && collar_bps < BPS_DENOMINATOR &&
		   "collar must be a non-negative fraction below 100%");

	if (collar_bps == NO_COLLAR) {
		// No band: everything on the grid above zero is admissible. A price of
		// 0 ticks is not an order, so the floor is one tick either way.
		collar_low_  = 1;
		collar_high_ = std::numeric_limits<price_t>::max() / 2;
		return;
	}

	// The multiply must not overflow: reference_scaled is bounded by the
	// venue's price range times 10^scale, and the factor is under 2x.
	assert(reference_scaled < std::numeric_limits<std::int64_t>::max() /
								  (BPS_DENOMINATOR + collar_bps) &&
		   "reference price too large for a collar at this scale");

	// Both roundings go inward, and both have to: the bps division and the tick
	// snap each drop a fraction, so the low edge needs ceiling at *both* steps
	// or the band silently widens below what the venue published.
	const std::int64_t low =
		ceil_to_tick(ceil_div(reference_scaled * (BPS_DENOMINATOR - collar_bps),
							  BPS_DENOMINATOR),
					 tick_scaled);
	const std::int64_t high = floor_to_tick(
		(reference_scaled * (BPS_DENOMINATOR + collar_bps)) / BPS_DENOMINATOR,
		tick_scaled);

	// A band narrower than one tick would admit nothing; clamp it to the
	// reference itself so the listing is degenerate rather than unusable.
	const std::int64_t low_ticks = low > 0 ? low / tick_scaled : 1;
	const std::int64_t high_ticks =
		high >= low ? high / tick_scaled : reference_scaled / tick_scaled;

	// An assertion rather than a rejection, like every other precondition here:
	// a listing whose own collar does not fit the engine's tick domain is a
	// reference-data fault, and every order on it would be refused all session.
	// Failing at startup is the only way an operator finds out in time.
	assert(high_ticks <=
			   static_cast<std::int64_t>(std::numeric_limits<price_t>::max()) &&
		   "collar spans more ticks than price_t can represent - the listing's "
		   "tick size is too fine for its price scale");

	collar_low_  = static_cast<price_t>(low_ticks);
	collar_high_ = static_cast<price_t>(high_ticks);
}

std::expected<price_t, reject_reason>
symbol_spec::price_from_scaled(std::int64_t scaled) const noexcept {
	if (scaled <= 0) return std::unexpected(reject_reason::MALFORMED_DECIMAL);
	// The whole point: no rounding mode, no nearest tick. Off the grid is a
	// rejection, exactly as Emporia's RoundingMode.UNNECESSARY divide throws.
	if (scaled % tick_scaled_ != 0)
		return std::unexpected(reject_reason::PRICE_NOT_ON_TICK);

	// This division is where a 64-bit scaled decimal becomes a 32-bit tick
	// count, and it is the only place in the engine where that narrowing
	// happens. A listing whose scale and tick put a legitimate price past
	// price_t would otherwise wrap it into a low tick - a price the book would
	// accept, sort and match at, with nothing to say it was ever wrong. So the
	// division is checked and the overflow refused, in the same voice as the
	// off-grid rejection above.
	const std::int64_t ticks = scaled / tick_scaled_;
	if (ticks > static_cast<std::int64_t>(std::numeric_limits<price_t>::max()))
		return std::unexpected(reject_reason::PRICE_OUT_OF_RANGE);
	return static_cast<price_t>(ticks);
}

std::expected<quantity_t, reject_reason>
symbol_spec::quantity_from_scaled(std::int64_t scaled) const noexcept {
	if (scaled <= 0)
		return std::unexpected(reject_reason::NON_POSITIVE_QUANTITY);
	if (scaled % lot_scaled_ != 0)
		return std::unexpected(reject_reason::QUANTITY_NOT_ON_LOT);

	// Same narrowing, same refusal. @see price_from_scaled - and note the check
	// is against one *order's* range: aggregates across orders are volume_t and
	// have room this deliberately does not.
	const std::int64_t lots = scaled / lot_scaled_;
	if (lots >
		static_cast<std::int64_t>(std::numeric_limits<quantity_t>::max()))
		return std::unexpected(reject_reason::QUANTITY_OUT_OF_RANGE);
	return static_cast<quantity_t>(lots);
}

std::expected<price_t, reject_reason>
symbol_spec::price_from_text(std::string_view text) const noexcept {
	const auto scaled = parse_exact_decimal(text, price_scale_);
	if (!scaled) return std::unexpected(scaled.error());
	return price_from_scaled(*scaled);
}

std::expected<quantity_t, reject_reason>
symbol_spec::quantity_from_text(std::string_view text) const noexcept {
	const auto scaled = parse_exact_decimal(text, qty_scale_);
	if (!scaled) return std::unexpected(scaled.error());
	return quantity_from_scaled(*scaled);
}

std::size_t symbol_spec::collar_span() const noexcept {
	if (!has_collar()) return 0;
	return static_cast<std::size_t>(collar_high_ - collar_low_) + 1;
}

std::expected<std::int64_t, reject_reason>
parse_exact_decimal(std::string_view text, int scale) noexcept {
	if (scale < 0 || scale > 18)
		return std::unexpected(reject_reason::MALFORMED_DECIMAL);
	if (text.empty()) return std::unexpected(reject_reason::MALFORMED_DECIMAL);

	std::size_t i = 0;
	if (text[i] == '+') ++i; // a sign is allowed but must not be negative
	if (i == text.size())
		return std::unexpected(reject_reason::MALFORMED_DECIMAL);

	constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
	const std::int64_t factor  = pow10(scale);

	std::int64_t integral   = 0;
	std::size_t digit_count = 0;
	for (; i < text.size() && text[i] != '.'; ++i) {
		const char c = text[i];
		if (c < '0' || c > '9')
			return std::unexpected(reject_reason::MALFORMED_DECIMAL);
		const auto digit = static_cast<std::int64_t>(c - '0');
		// Checked before the multiply so an overflowing price is a rejection
		// rather than a wrapped value that lands on the tick grid by accident.
		if (integral > (MAX - digit) / 10)
			return std::unexpected(reject_reason::MALFORMED_DECIMAL);
		integral = integral * 10 + digit;
		++digit_count;
	}

	std::int64_t fraction   = 0;
	std::size_t frac_digits = 0;
	if (i < text.size()) {
		++i; // skip '.'
		for (; i < text.size(); ++i) {
			const char c = text[i];
			if (c < '0' || c > '9')
				return std::unexpected(reject_reason::MALFORMED_DECIMAL);
			// The strictness this whole function exists for: a digit past the
			// listing's scale cannot be represented, so it is refused instead
			// of dropped. market_data's parse_fixed_point truncates here.
			if (frac_digits == static_cast<std::size_t>(scale))
				return std::unexpected(reject_reason::MALFORMED_DECIMAL);
			fraction = fraction * 10 + (c - '0');
			++frac_digits;
			++digit_count;
		}
	}

	if (digit_count == 0)
		return std::unexpected(reject_reason::MALFORMED_DECIMAL);

	// Zero-pad a short fraction: "1.5" at scale 3 is 1500, not 15.
	for (std::size_t pad = frac_digits; pad < static_cast<std::size_t>(scale);
		 ++pad)
		fraction *= 10;

	if (integral > (MAX - fraction) / factor)
		return std::unexpected(reject_reason::MALFORMED_DECIMAL);
	return integral * factor + fraction;
}

[[nodiscard]] symbol_id_t symbol_spec::id() const noexcept { return id_; }

[[nodiscard]] std::string_view symbol_spec::symbol() const noexcept {
	return symbol_;
}

[[nodiscard]] int symbol_spec::price_scale() const noexcept {
	return price_scale_;
}

[[nodiscard]] int symbol_spec::qty_scale() const noexcept { return qty_scale_; }

[[nodiscard]] std::int64_t symbol_spec::tick_scaled() const noexcept {
	return tick_scaled_;
}

[[nodiscard]] std::int64_t symbol_spec::lot_scaled() const noexcept {
	return lot_scaled_;
}

[[nodiscard]] std::int64_t
symbol_spec::price_to_scaled(price_t ticks) const noexcept {
	return static_cast<std::int64_t>(ticks) * tick_scaled_;
}

[[nodiscard]] std::int64_t
symbol_spec::quantity_to_scaled(quantity_t lots) const noexcept {
	return lots * lot_scaled_;
}

[[nodiscard]] bool symbol_spec::has_collar() const noexcept {
	return collar_bps_ != NO_COLLAR;
}

[[nodiscard]] price_t symbol_spec::collar_low() const noexcept {
	return collar_low_;
}

[[nodiscard]] price_t symbol_spec::collar_high() const noexcept {
	return collar_high_;
}

[[nodiscard]] bool symbol_spec::within_collar(price_t ticks) const noexcept {
	return ticks >= collar_low_ && ticks <= collar_high_;
}

[[nodiscard]] std::size_t
symbol_spec::indexed_side_bytes(std::size_t level_bytes) const noexcept {
	return collar_span() * level_bytes;
}

[[nodiscard]] std::size_t
symbol_spec::tick_index(price_t ticks) const noexcept {
	return static_cast<std::size_t>(ticks - collar_low_);
}

[[nodiscard]] price_t
symbol_spec::price_at_index(std::size_t index) const noexcept {
	return collar_low_ + static_cast<price_t>(index);
}
} // namespace exchange::engine

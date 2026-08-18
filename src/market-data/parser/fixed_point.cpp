#include "fixed_point.hpp"

#include "detail/digits.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <limits>

namespace exchange::market_data::parser {
namespace {

[[nodiscard]] constexpr bool is_digit(char c) noexcept {
	return static_cast<unsigned char>(c) >= '0' &&
		   static_cast<unsigned char>(c) <= '9';
}

/**
 * @brief @c value = @c value*mul + add, rejecting int64 overflow.
 * @pre @p value, @p mul and @p add are all non-negative (the sign is applied
 *      last, after accumulation).
 * @return false if the result would exceed @c INT64_MAX.
 */
[[nodiscard]] constexpr bool mul_add(std::int64_t &value, std::int64_t mul,
									 std::int64_t add) noexcept {
	if (value > (std::numeric_limits<std::int64_t>::max() - add) / mul)
		return false;
	value = value * mul + add;
	return true;
}

/**
 * @brief Accumulate a run of ASCII digits starting at @p p into @p value.
 *
 * Consumes digits until the first non-digit, @p end, or (when @p cap is
 * non-negative) until @p cap digits have been taken - whichever comes first.
 * Advances @p p past what it consumes, sets @p any_digit if it took at least
 * one, and adds the count to @p consumed_out. Wide loads fold 16/8 digits at a
 * time; a scalar loop finishes the tail.
 * @post @p p is never advanced past @p end (no over-read).
 * @return @c parse_error::overflow if the value would exceed int64, else empty.
 */
[[nodiscard]] std::optional<parse_error>
consume_digits(const char *&p, const char *end, int cap, std::int64_t &value,
			   bool &any_digit, int &consumed_out) {
	int consumed        = 0;
	const auto room     = [&](int n) { return cap < 0 || consumed + n <= cap; };
	const auto has_room = [&] { return cap < 0 || consumed < cap; };

	if constexpr (detail::SWAR_NATIVE) {
#ifdef PARSER_DETAIL_HAS_SSE41
		while (room(16) && end - p >= 16 && detail::is_sixteen_digits(p)) {
			if (!mul_add(
					value,
					10'000'000'000'000'000LL,
					static_cast<std::int64_t>(detail::parse_sixteen_digits(p))))
				return parse_error::overflow;
			p += 16;
			consumed += 16;
			any_digit = true;
		}
#endif
		while (room(8) && end - p >= 8) {
			std::uint64_t word;
			std::memcpy(&word, p, sizeof word);
			if (!detail::is_eight_digits(word)) break;
			if (!mul_add(value,
						 100'000'000LL,
						 detail::parse_eight_digits(word)))
				return parse_error::overflow;
			p += 8;
			consumed += 8;
			any_digit = true;
		}
	}

	while (has_room() && p != end && is_digit(*p)) {
		if (!mul_add(value, 10, *p - '0')) return parse_error::overflow;
		++p;
		++consumed;
		any_digit = true;
	}

	consumed_out += consumed;
	return std::nullopt;
}

/// @brief Advance @p p over a maximal run of ASCII digits (validate-and-drop).
/// @post @p p is never advanced past @p end.
void skip_digits(const char *&p, const char *end) noexcept {
	if constexpr (detail::SWAR_NATIVE) {
		while (end - p >= 8) {
			std::uint64_t word;
			std::memcpy(&word, p, sizeof word);
			if (!detail::is_eight_digits(word)) break;
			p += 8;
		}
	}
	while (p != end && is_digit(*p)) ++p;
}

} // namespace

std::expected<std::int64_t, parse_error>
parse_fixed_point(std::string_view text, int scale) noexcept {
	if (scale < 0) return std::unexpected(parse_error::negative_scale);
	if (text.empty()) return std::unexpected(parse_error::empty);

	const char *p         = text.data();
	const char *const end = p + text.size();

	bool negative = false;
	if (*p == '+' || *p == '-') {
		negative = *p == '-';
		++p;
	}

	std::int64_t value{0};
	bool any_digit = false;
	int consumed   = 0; // fractional digits actually folded into value

	// Integer part: an unbounded digit run stopping at '.' or end.
	int ignored = 0;
	if (const auto err = consume_digits(p, end, -1, value, any_digit, ignored);
		err)
		return std::unexpected(*err);

	if (p != end && *p == '.') {
		++p;
		// Fractional part: fold up to `scale` digits, then validate-and-drop
		// any surplus precision the venue sent.
		if (const auto err =
				consume_digits(p, end, scale, value, any_digit, consumed);
			err)
			return std::unexpected(*err);
		skip_digits(p, end);
	}

	// Anything left is a stray byte the grammar does not allow (a second '.', a
	// letter, a trailing sign, ...).
	if (p != end) return std::unexpected(parse_error::invalid_char);
	if (!any_digit) return std::unexpected(parse_error::no_digits);

	// Zero-pad a short fraction up to the requested scale.
	for (; consumed < scale; ++consumed)
		if (!mul_add(value, 10, 0))
			return std::unexpected(parse_error::overflow);

	return negative ? -value : value;
}

} // namespace exchange::market_data::parser

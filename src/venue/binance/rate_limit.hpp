#pragma once
// Reading the venue's own rate-limit count off a response.
//
// `weight_budget` estimates what has been spent; Binance states it, on every
// response, in a header. The estimate drifts - a request retried, a weight
// guessed wrong, another process on the same address - and this is the only
// number that does not. @see weight_budget::reconcile
//
// --- why this takes a value and not a response -----------------------------
//
// Because `venue/` has no transport edge, deliberately: `market_data/` depends
// on this module and has been written since it was born not to depend on
// `transport/`, so a `transport::rest::reply` parameter here would hand it one
// transitively. The caller already has the header list and a case-insensitive
// `find_header` to search it with, so the split is one line at the call site:
//
//     if (const auto raw = find_header(reply.headers, USED_WEIGHT_HEADER))
//         budget.reconcile(parse_used_weight(*raw).value_or(0), now);
//
// What stays here is the pair that is *venue* knowledge - the header's name and
// the format of its value - rather than being split across two modules.

#include "venue_export.hpp" // VENUE_EXPORT (generated)

#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace exchange::venue::binance {

/**
 * @brief The header carrying weight used in the current minute.
 *
 * @note Match it case-insensitively. HTTP field names are (RFC 9110 §5.1) and
 *       Binance is not consistent about the capitalisation of this one across
 *       endpoints - an exact comparison is how the header goes quietly missing
 *       and the budget silently stops being reconciled.
 */
inline constexpr std::string_view USED_WEIGHT_HEADER = "X-MBX-USED-WEIGHT-1M";

/**
 * @brief The header carrying order count used in the current ten seconds.
 *
 * A second, independent limit: Binance caps orders per interval as well as
 * weight per minute, and an account can exhaust this one while its IP weight
 * budget is barely touched.
 */
inline constexpr std::string_view USED_ORDER_COUNT_HEADER =
	"X-MBX-ORDER-COUNT-10S";

/**
 * @brief @p value as a count, or nothing if it is not one.
 *
 * @param value The header's value, exactly as received.
 * @return The count, or @c std::nullopt when the header is absent-shaped
 *         (empty), carries something that is not a number, or carries trailing
 *         characters.
 *
 * @note Nothing rather than zero on a malformed value. Zero would read as "the
 *       budget is completely fresh", which is the most dangerous possible
 *       misreading of a header we failed to understand - it would licence a
 *       full minute's traffic on no evidence. The caller keeps its own estimate
 *       instead, which errs towards sending less.
 */
[[nodiscard]] constexpr std::optional<int>
parse_used_weight(std::string_view value) noexcept {
	if (value.empty()) return std::nullopt;
	int count             = 0;
	const char *const end = value.data() + value.size();
	const auto [stop, ec] = std::from_chars(value.data(), end, count);
	if (ec != std::errc{} || stop != end || count < 0) return std::nullopt;
	return count;
}

} // namespace exchange::venue::binance

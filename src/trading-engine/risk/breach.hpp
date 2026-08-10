#pragma once
// What the gate found wrong, as a set of bits rather than a first answer.
//
// The whole point of the bitmask is that the checks do not short-circuit. A
// chain of `if (...) return reason;` costs one unpredictable branch per rule
// and stops at the first hit; this evaluates every rule into a register, ORs
// the results together, and takes exactly one branch on the total. Ten rules
// therefore cost ten compares and one branch, not ten branches — and the mask
// also happens to be the *complete* answer, which is what an operator wants
// when an order is refused for three reasons at once.

#include "core/util/enum_string.hpp"
#include "core/util/flag.hpp"
#include "fwd.hpp"
#include "trading-engine/order_book/reject_reason.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace exchange::engine::risk {

/**
 * @brief The rules, their bits and their descriptions, in one list.
 *
 * A *valued* X-macro list — @c X(name, value, label) — because these values are
 * bits and the ordinary list would number them 0, 1, 2. Everything below is
 * generated from it: the enum body, both string accessors, the all-bits mask
 * and the bit count. Adding a rule means adding one line here and one case to
 * @c reason_for, and the compiler insists on the second.
 *
 * @par Order is severity order, and that is load-bearing
 * A refused command reports one @c reject_reason, and the one it reports is the
 * lowest set bit. So the rules are listed worst-first: @c HALTED outranks
 * everything because "we are not trading" is a truer answer than "that order
 * was large", and a malformed quantity outranks a limit breach because a
 * negative size makes every number downstream of it meaningless. Reordering
 * these changes what clients are told; the mask itself is order-independent,
 * and
 * @c breach_set carries all of it for anyone who wants the rest.
 */
#define RISK_BREACH_LIST(X)                                                    \
	X(NONE, 0U, "no rule was broken")                                          \
	X(HALTED,                                                                  \
	  1U << 0U,                                                                \
	  "the circuit breaker is not passing this kind of command")               \
	X(NON_POSITIVE_QUANTITY, 1U << 1U, "quantity was zero or negative")        \
	X(ORDER_QUANTITY, 1U << 2U, "quantity exceeds the per-order limit")        \
	X(ORDER_NOTIONAL,                                                          \
	  1U << 3U,                                                                \
	  "price times quantity exceeds the per-order limit")                      \
	X(PRICE_BAND, 1U << 4U, "price is outside the band around the last print") \
	X(POSITION_LIMIT,                                                          \
	  1U << 5U,                                                                \
	  "the resulting net position would exceed its limit")                     \
	X(EXPOSURE_LIMIT,                                                          \
	  1U << 6U,                                                                \
	  "resulting gross exposure would exceed its limit")                       \
	X(WORKING_ORDERS, 1U << 7U, "the working-order ledger is at its limit")    \
	X(DUPLICATE_ORDER, 1U << 8U, "an order with this id is already working")   \
	X(MESSAGE_RATE, 1U << 9U, "this window's message allowance is spent")

/// @brief One risk rule, as a single bit. @see RISK_BREACH_LIST
enum class breach : std::uint32_t {
	EXCHANGE_ENUM_VALUED_VALUES(RISK_BREACH_LIST)
};

EXCHANGE_ENABLE_FLAGS(breach)

/// @brief The enumerator name of @p value, e.g. @c "PRICE_BAND".
EXCHANGE_ENUM_VALUED_NAME(breach, to_string, RISK_BREACH_LIST)

/// @brief A short description of the rule @p value names, for logs.
EXCHANGE_ENUM_VALUED_LABEL_ONLY(breach, describe, RISK_BREACH_LIST)

/// @brief A set of broken rules — possibly empty, possibly several at once.
using breach_set = core::util::flag<breach>;

// Derived from the list rather than written beside it, so a new rule cannot be
// added without the mask and the count following it.
#define RISK_BREACH_OR_BIT(name, value, label) | (value)

/// @brief Every bit @c breach defines, ORed together — the mask of "any rule".
inline constexpr std::uint32_t BREACH_ALL_BITS =
	0U EXCHANGE_ENUM_VALUED_FOR_EACH(RISK_BREACH_LIST, RISK_BREACH_OR_BIT);

#undef RISK_BREACH_OR_BIT

/// @brief One past the highest bit index @c breach uses — the reason table's
///        length.
inline constexpr std::size_t BREACH_BIT_COUNT =
	static_cast<std::size_t>(std::bit_width(BREACH_ALL_BITS));

/**
 * @brief The @c reject_reason a client is told when @p bit is the reason.
 *
 * A switch rather than a fourth column on the list, and deliberately: the
 * compiler enforces coverage of a switch, so adding a @c breach without
 * deciding what a client hears is a warning here — and a warning is an error
 * under the project's warning set. A macro column would silently accept a
 * blank.
 *
 * @note Two breaches reuse reasons the rest of the engine already defines.
 *       @c NON_POSITIVE_QUANTITY and @c DUPLICATE_ORDER are refusals the book
 *       would also make — the gate just makes them earlier, before the command
 *       occupies a queue slot — and a client should not be able to tell which
 *       boundary answered.
 */
[[nodiscard]] constexpr reject_reason reason_for(breach bit) noexcept {
	switch (bit) {
	case breach::NONE: return reject_reason::NONE;
	case breach::HALTED: return reject_reason::RISK_HALTED;
	case breach::NON_POSITIVE_QUANTITY:
		return reject_reason::NON_POSITIVE_QUANTITY;
	case breach::ORDER_QUANTITY: return reject_reason::RISK_ORDER_QUANTITY;
	case breach::ORDER_NOTIONAL: return reject_reason::RISK_ORDER_NOTIONAL;
	case breach::PRICE_BAND: return reject_reason::RISK_PRICE_BAND;
	case breach::POSITION_LIMIT: return reject_reason::RISK_POSITION_LIMIT;
	case breach::EXPOSURE_LIMIT: return reject_reason::RISK_EXPOSURE_LIMIT;
	case breach::WORKING_ORDERS: return reject_reason::RISK_WORKING_ORDERS;
	case breach::DUPLICATE_ORDER: return reject_reason::DUPLICATE_ORDER_ID;
	case breach::MESSAGE_RATE: return reject_reason::RISK_MESSAGE_RATE;
	}
	return reject_reason::NONE;
}

/**
 * @brief Bit index to reason, built at compile time from @c reason_for.
 *
 * Generated rather than written out, so there is exactly one place that decides
 * what a breach means and no parallel array to fall out of step with it. The
 * lookup on the reporting path is then a @c countr_zero and a load from a
 * ten-byte constant.
 */
inline constexpr std::array<reject_reason, BREACH_BIT_COUNT> REASON_BY_BIT =
	[] {
		std::array<reject_reason, BREACH_BIT_COUNT> table{};
		for (std::size_t i = 0; i < BREACH_BIT_COUNT; ++i)
			table[i] = reason_for(static_cast<breach>(std::uint32_t{1} << i));
		return table;
	}();

// Every rule owns exactly one bit and no two share one. Without this a typo in
// the list — two rules at `1U << 7U` — would compile as an alias and quietly
// report the wrong reason for one of them.
static_assert(std::popcount(BREACH_ALL_BITS) ==
				  static_cast<int>(BREACH_BIT_COUNT),
			  "the breach bits must be distinct and contiguous from bit 0");

/**
 * @brief The single reason to report for @p breaches — the lowest set bit.
 * @return @c NONE when @p breaches is empty.
 * @see RISK_BREACH_LIST for why "lowest" means "most severe".
 */
[[nodiscard]] constexpr reject_reason
first_reason(breach_set breaches) noexcept {
	const std::uint32_t bits = breaches.bits();
	if (bits == 0) return reject_reason::NONE;
	const auto index = static_cast<std::size_t>(std::countr_zero(bits));
	// Bits above the last enumerator cannot be produced by the gate; a caller
	// that hand-built a mask out of from_bits gets NONE rather than a read past
	// the table.
	return index < BREACH_BIT_COUNT ? REASON_BY_BIT[index]
									: reject_reason::NONE;
}

} // namespace exchange::engine::risk

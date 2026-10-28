#pragma once
// What the gate is willing to let through, and why it stopped.
//
// Split from circuit_breaker.hpp because these are what the breaker *says*, and
// most of the code that reads them never touches the breaker itself: the gate
// screens against a trading_state on every command, and an operator's console
// reports a trip_cause without owning anything. Both print as their enumerator
// names; describe() says what each one means.

#include "core/util/enum_format.hpp"
#include "fwd.hpp"

#include <cstdint>
#include <string_view>

namespace exchange::risk::hooks::system {

using core::util::format_as;

/**
 * @brief What the gate is currently willing to let through.
 *
 * @par Why @c CANCEL_ONLY is the interesting state and @c HALTED is not
 * A kill switch that blocks everything also blocks the *withdrawals* - it
 * freezes a malfunctioning strategy's orders in the book and leaves them there
 * to be filled by whoever noticed. That is the wrong emergency behaviour, and
 * it is why every real venue's halt still accepts cancels. @c CANCEL_ONLY is
 * therefore what the automatic trip selects: stop adding risk, keep the ability
 * to shed it.
 *
 * @c HALTED exists for the narrower case where the strategy itself is not
 * trusted to name the right orders - a bad deploy sending cancels for ids it
 * invented, say. Then the correct action really is silence, and the positions
 * are unwound by hand from the other side. It is never selected automatically.
 */
enum class trading_state : std::uint8_t { NORMAL, CANCEL_ONLY, HALTED };

/// @brief What the gate passes in @p state, for logs.
[[nodiscard]] constexpr std::string_view
describe(trading_state state) noexcept {
	switch (state) {
		using enum trading_state;
	case NORMAL: return "every command passes the ordinary checks";
	case CANCEL_ONLY: return "risk-reducing commands only; no new liquidity";
	case HALTED: return "nothing passes, cancels included";
	}
	return {};
}

/**
 * @brief Why the breaker last left @c NORMAL.
 *
 * The state says trading stopped; this says what to do about it, and they are
 * different questions. A @c BREACH_RATE trip means a strategy is malfunctioning
 * and someone should read its logs before re-arming. A @c LOSS_LIMIT trip means
 * the strategy is working exactly as written and losing money, which is a
 * decision for a human, not a bug. A @c STALE_FEED trip means nothing is wrong
 * with the strategy at all and the fault is in the link, which makes it the one
 * cause whose fix is not in this process. Re-arming blindly is the wrong
 * response to all three, for three different reasons - so the cause is recorded
 * rather than left to be inferred from whatever else happened to be on screen.
 *
 * @par The post-trade four
 * The last four are the same argument one lane further on. None of them is a
 * property of any order - they are properties of the *stream of what came back*
 * - so none of them could have been a pre-trade rule, and each says something
 * different about what to do next. @c ORDER_TRADE_RATIO is a strategy quoting
 * without trading, which is a venue-relations problem before it is a risk one.
 * @c FILL_BURST is being filled faster than anybody intended, which is usually
 * a stale quote. @c ADVERSE_RUN is the market running through a resting side,
 * which may be the strategy working as written into a move it should not be in.
 * @c STALE_WORKING is exposure the process believes in and has heard nothing
 * about, which is the one where the position on screen may not be real - so it
 * is the one to reconcile against the venue before doing anything else. @see
 * hooks/post_trade/fwd.hpp
 */
enum class trip_cause : std::uint8_t {
	NONE,
	OPERATOR,
	BREACH_RATE,
	LOSS_LIMIT,
	STALE_FEED,
	ORDER_TRADE_RATIO,
	FILL_BURST,
	ADVERSE_RUN,
	STALE_WORKING,
};

/// @brief What tripped the breaker when @p cause did, for logs.
[[nodiscard]] constexpr std::string_view describe(trip_cause cause) noexcept {
	switch (cause) {
		using enum trip_cause;
	case NONE: return "the breaker has not tripped";
	case OPERATOR: return "somebody threw the switch";
	case BREACH_RATE:
		return "too many refusals in one window - a looping strategy";
	case LOSS_LIMIT: return "realised plus unrealised loss passed its floor";
	case STALE_FEED: return "the venue has gone quiet - market data is stale";
	case ORDER_TRADE_RATIO:
		return "too many messages per execution - a quoting loop";
	case FILL_BURST:
		return "executions arriving faster than a strategy can have meant";
	case ADVERSE_RUN: return "the tape has run one way through our prints";
	case STALE_WORKING:
		return "orders working and the order return path silent";
	}
	return {};
}

} // namespace exchange::risk::hooks::system
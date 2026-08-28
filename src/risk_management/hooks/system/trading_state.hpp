#pragma once
// What the gate is willing to let through, and why it stopped.
//
// Split from circuit_breaker.hpp because these are what the breaker *says*, and
// most of the code that reads them never touches the breaker itself: the gate
// screens against a trading_state on every command, and an operator's console
// reports a trip_cause without owning anything. The X-macro list that generates
// an enum's names is part of the enum and travels with it.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <cstdint>


namespace exchange::risk::hooks::system {

#define RISK_TRADING_STATE_LIST(X)                                             \
	X(NORMAL, "every command passes the ordinary checks")                      \
	X(CANCEL_ONLY, "risk-reducing commands only; no new liquidity")            \
	X(HALTED, "nothing passes, cancels included")

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
enum class trading_state : std::uint8_t {
	EXCHANGE_ENUM_VALUES(RISK_TRADING_STATE_LIST)
};

EXCHANGE_ENUM_NAME(trading_state, to_string, RISK_TRADING_STATE_LIST)

EXCHANGE_ENUM_LABEL_ONLY(trading_state, describe, RISK_TRADING_STATE_LIST)

#define RISK_TRIP_CAUSE_LIST(X)                                                \
	X(NONE, "the breaker has not tripped")                                     \
	X(OPERATOR, "somebody threw the switch")                                   \
	X(BREACH_RATE, "too many refusals in one window - a looping strategy")     \
	X(LOSS_LIMIT, "realised plus unrealised loss passed its floor")            \
	X(STALE_FEED, "the venue has gone quiet - market data is stale")           \
	X(ORDER_TRADE_RATIO, "too many messages per execution - a quoting loop")   \
	X(FILL_BURST, "executions arriving faster than a strategy can have meant") \
	X(ADVERSE_RUN, "the tape has run one way through our prints")              \
	X(STALE_WORKING, "orders working and the order return path silent")

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
	EXCHANGE_ENUM_VALUES(RISK_TRIP_CAUSE_LIST)
};

EXCHANGE_ENUM_NAME(trip_cause, to_string, RISK_TRIP_CAUSE_LIST)

EXCHANGE_ENUM_LABEL_ONLY(trip_cause, describe, RISK_TRIP_CAUSE_LIST)


#undef RISK_TRADING_STATE_LIST
#undef RISK_TRIP_CAUSE_LIST
} // namespace exchange::risk::hooks::system
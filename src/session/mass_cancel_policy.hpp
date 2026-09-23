#include "core/util/enum_string.hpp"

#include <cstdint>

namespace exchange::session {
#define SESSION_MASS_CANCEL_POLICY_LIST(X)                                     \
	X(MANUAL, "never on its own - an operator calls mass_cancel()")            \
	X(ON_HALT, "withdraw the ledger when the breaker reaches HALTED")          \
	X(ON_ANY_TRIP, "withdraw it on any departure from NORMAL")

/**
 * @brief When a session withdraws the gate's whole ledger without being asked.
 *
 * @par Why this is a choice and not a default behaviour
 * Because the three answers are each right for a different deployment, and the
 * cost of the wrong one is asymmetric in both directions. @c ON_ANY_TRIP pulls
 * the book on a drawdown, a stale feed or a looping strategy - correct for an
 * unattended run, and an over-reaction for a desk that expects @c CANCEL_ONLY
 * to mean "stop adding, keep quoting what is there". @c MANUAL leaves live
 * quotes in a book nobody is managing until a human acts, which is the right
 * posture only when a human is actually watching.
 *
 * @c ON_HALT is the middle, and the one that matches what the two states
 * already mean: @c CANCEL_ONLY is the automatic trip and says "shed, do not
 * add", so the strategy is still trusted to manage its own orders and pulling
 * them out from under it would be wrong. @c HALTED is the hand-selected state
 * that says the strategy is *not* trusted, and that is exactly when something
 * other than the strategy has to do the withdrawing.
 *
 * @note Whichever is chosen, the walk is @c risk_gate::mass_cancel - the gate's
 *       ledger, not the quoter's view. That is deliberate and it is the
 *       difference from @c live_session::withdraw_all: an orderly shutdown
 *       trusts the quoter to say what it has live, and an emergency does not
 *       trust the strategy at all. @see live_session::withdraw_all
 */
enum class mass_cancel_policy : std::uint8_t {
	EXCHANGE_ENUM_VALUES(SESSION_MASS_CANCEL_POLICY_LIST)
};

EXCHANGE_ENUM_NAME(mass_cancel_policy, to_string,
				   SESSION_MASS_CANCEL_POLICY_LIST)

EXCHANGE_ENUM_LABEL_ONLY(mass_cancel_policy, describe,
						 SESSION_MASS_CANCEL_POLICY_LIST)
} // namespace exchange::session
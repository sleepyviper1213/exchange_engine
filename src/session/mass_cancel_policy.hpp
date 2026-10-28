#pragma once
#include "core/util/enum_format.hpp"

#include <cstdint>
#include <string_view>

namespace exchange::session {

using core::util::format_as;

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
enum class mass_cancel_policy : std::uint8_t { MANUAL, ON_HALT, ON_ANY_TRIP };

/// @brief When @p policy withdraws the ledger, for logs. The enum itself
///        prints as its enumerator name, e.g. @c "ON_HALT".
[[nodiscard]] constexpr std::string_view
describe(mass_cancel_policy policy) noexcept {
	switch (policy) {
		using enum mass_cancel_policy;
	case MANUAL: return "never on its own - an operator calls mass_cancel()";
	case ON_HALT: return "withdraw the ledger when the breaker reaches HALTED";
	case ON_ANY_TRIP: return "withdraw it on any departure from NORMAL";
	}
	return {};
}

} // namespace exchange::session
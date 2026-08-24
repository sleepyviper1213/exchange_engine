#pragma once
// The hooks' scaffolding: the state a batch carries into them, and the trick
// that turns one rule's answer into a bit of a mask.
//
// Neither is anything a caller of `risk_gate` names. They live here rather than
// in the gate's private section because the gate is a template - every private
// member of a template is in the public header whether it is part of the
// interface or not, so `detail` is the only place that can say it is not.
//
// The fat-finger band used to live here too. It moved to
// `hooks/pre_trade/price_collar.hpp` when the rules were given homes: a band is
// what one hook is *about*, so keeping it here made the one rule whose state is
// worth reading the one rule a caller could not name.

#include "../../clock.hpp"
#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/system/trading_state.hpp"
#include "orders/types.hpp"

#include <cstdint>

namespace exchange::risk::hooks::detail {

/**
 * @brief @p rule's bit when @p failed, zero otherwise - with no branch.
 *
 * Negating a @c bool gives all-ones or all-zeros, and the AND then either keeps
 * the bit or drops it. This is the whole trick behind @c breach_set, and it is
 * why ten rules cost one branch between them rather than ten.
 */
[[nodiscard]] constexpr breach_bits bit_if(bool failed, breach rule) noexcept {
	return static_cast<breach_bits>(static_cast<unsigned>(rule) &
									-static_cast<unsigned>(failed));
}

/**
 * @brief Everything hoisted out of the per-command loop, plus what the batch
 *        has provisionally used up so far.
 *
 * Read once in @c risk_gate::open_batch and then only from registers: the
 * clock, the breaker's state, the rate window's headroom and the position are
 * all things the rules would otherwise reload per command, and this thread is
 * the only writer of the last of them. The three @c pending fields are what
 * makes a batch screen against itself - a hundred orders in one call may not
 * each be sized against the position the batch started from.
 */
struct screen_state {
	/// @brief The batch's single clock reading, from the gate's injected clock.
	monotonic_time now;
	system::trading_state state;
	std::uint32_t headroom;    ///< messages still allowed this window
	volume_t base_net;         ///< position at batch start
	volume_t base_working_bid; ///< working buys at batch start
	volume_t base_working_ask; ///< working sells at batch start
	volume_t pending_bid  = 0; ///< buys this batch has added
	volume_t pending_ask  = 0; ///< sells this batch has added
	std::uint32_t charged = 0; ///< messages this batch has used
};

} // namespace exchange::risk::hooks::detail

// Offered to the rules by their own namespace, so a rule writes `bit_if(...)`
// and `const screen_state &` with no prefix at all.
//
// Not cosmetic: `hooks::pre_trade::detail` also exists - it holds the probe
// table under the ledger and the padded entry under the position book - and it
// shadows this one from inside a rule, so a bare `detail::bit_if` there
// resolves to the wrong namespace and fails to compile. Spelling
// `hooks::detail::` at every call site works and reads like an accident waiting
// to be tidied away; making the short spelling the correct one removes the trap
// instead of documenting it.
namespace exchange::risk::hooks::pre_trade {
using hooks::detail::bit_if;
using hooks::detail::screen_state;
} // namespace exchange::risk::hooks::pre_trade

namespace exchange::risk::hooks::system {
using hooks::detail::bit_if;
} // namespace exchange::risk::hooks::system

#pragma once
// Rate Limiter Hook: how many messages this producer may send, how that
// question is answered without a division, and the one compare that refuses an
// order.
//
// The hook that catches a strategy stuck in a loop before the venue's own
// throttle does - being disconnected for exceeding a message rate is a far
// worse outcome than refusing the order yourself, because the disconnection
// takes the cancels with it.
//
// The window and the rule read against it are one file because they are one
// decision split only by *when* it is made: the window is asked for its
// headroom once per batch, and the rule then compares against what the batch
// has spent. Two files made that look like two hooks and put the sentence
// explaining the split in both of them.

// The macro is used below, so it is included here rather than inherited from a
// fwd header: a declaration-only header does not "use" it, and an include
// cleaner that judges it unused there would strip every export annotation in
// this file.
#include "risk_management/hooks/breach.hpp"
#include "risk_management/hooks/detail/screening.hpp" // bit_if, screen_state
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)

#include <cstdint>

namespace exchange::risk::hooks::pre_trade {

/**
 * @brief A fixed-window message counter: the epoch is a shift of the clock, so
 *        asking costs a shift, a compare and an AND.
 *
 * @par Why not a token bucket
 * A token bucket is the textbook answer and it is the wrong one here, for one
 * reason: refilling means @c elapsed_ns / @c ns_per_token, and an integer
 * division is twenty to forty cycles on a path that is supposed to cost a
 * handful in total. Every trick for avoiding it - a reciprocal multiply, a
 * pre-scaled tick counter - is a way of making the window a power of two, and
 * once the window is a power of two you may as well index by it directly.
 *
 * So the window is @c 2^window_log2_ns nanoseconds and the current window's
 * index is @c now_ns >> window_log2_ns. A shift. Nothing else on the path
 * touches the clock's magnitude at all.
 *
 * @par What that costs, stated honestly
 * A fixed window admits up to @c 2*limit messages across a window boundary -
 * @c limit at the end of one and @c limit at the start of the next - where a
 * token bucket would admit @c limit plus the trickle. That is a real difference
 * and it is why the window wants to be *short*: the default of @c 2^20 ns is
 * about 1.05 ms, so the worst-case burst is bounded by two windows' allowance
 * inside a couple of milliseconds, which is well under what a gateway's own
 * buffer absorbs. Set a one-second window and the burst becomes a second's
 * worth of traffic arriving at once, which is exactly the thing the limit
 * existed to prevent.
 *
 * @par Not atomic, and that is the design
 * A rate limit protects a gateway from *one producer*, and a producer is one
 * thread - the same thread that owns the SPSC queue's producer side. Sharing
 * one limiter across threads would need a @c fetch_add per message and would be
 * measuring something nobody asked about. A firm-wide cap belongs one level up,
 * beside the aggregation that can afford it.
 *
 * @par Screening and charging are separate calls
 * @c headroom asks and changes nothing; @c charge commits. The gate needs that
 * split because it screens a whole batch before it knows whether the batch will
 * be accepted downstream - see @c risk_gate on why nothing is committed until
 * the sink has taken the commands.
 */
class rate_limiter {
public:
	/// @brief About 1.05 ms. @see the class note on why short windows matter.
	static constexpr unsigned DEFAULT_WINDOW_LOG2_NS = 20;

	/// @brief Widest window the shift may name - a shift of 64 or more is
	///        undefined, and anything close is a window longer than a session.
	static constexpr unsigned MAX_WINDOW_LOG2_NS = 40; // ~18 minutes

	/**
	 * @brief A limiter admitting @p max_per_window messages per window.
	 * @param max_per_window Allowance. Zero admits nothing
	 * @param window_log2_ns Base-2 log of the window in nanoseconds
	 * @pre at most @c MAX_WINDOW_LOG2_NS.
	 */
	RISK_MANAGEMENT_EXPORT explicit rate_limiter(
		std::uint32_t max_per_window,
		unsigned window_log2_ns = DEFAULT_WINDOW_LOG2_NS) noexcept;

	/// @brief The allowance per window.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint32_t limit() const noexcept;

	/// @brief The window's width in nanoseconds.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	window_ns() const noexcept;

	/**
	 * @brief Messages already charged in the window @p now_ns falls in.
	 *
	 * The branchless half: @c -(epoch == epoch_) is all-ones when the stored
	 * count still belongs to this window and zero when it belongs to a past
	 * one, so the AND either keeps the count or produces a fresh zero. No
	 * branch, and no need to have noticed the rollover beforehand - a limiter
	 * left untouched for an hour reports zero used the moment it is asked.
	 */
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint32_t
	used(std::uint64_t now_ns) const noexcept;

	/// @brief How many more messages fit in @p now_ns's window. Pure.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint32_t
	headroom(std::uint64_t now_ns) const noexcept;

	/// @brief Whether @p count more messages would fit. Pure.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	admits(std::uint64_t now_ns, std::uint32_t count) const noexcept;

	/**
	 * @brief Charge @p count messages against @p now_ns's window.
	 *
	 * Unchecked: the caller screened with @c headroom and is committing what it
	 * was told fits. Charging past the limit saturates rather than wrapping, so
	 * a caller that skipped the screen throttles itself instead of unlocking a
	 * full window.
	 */

	RISK_MANAGEMENT_EXPORT void charge(std::uint64_t now_ns,
									   std::uint32_t count) noexcept;

	/// @brief Forget the current window. A session boundary, or a test.

	RISK_MANAGEMENT_EXPORT void reset() noexcept;

private:
	std::uint32_t limit_;
	unsigned shift_;
	/// @brief Which window @c used_ belongs to. Zero is a real epoch - the one
	///        containing t=0 - and that is harmless: @c used_ starts at zero
	///        too, so a fresh limiter reads as unused either way.
	std::uint64_t epoch_ = 0;
	std::uint32_t used_  = 0;
};

/**
 * @brief Whether this batch has already spent its window's allowance.
 *
 * @return @c MESSAGE_RATE, or zero.
 *
 * @par Why the rule does not touch the limiter
 * Because the reading it needs was taken once, when the batch opened:
 * @c state.headroom is what @c headroom returned then and @c state.charged is
 * what the commands ahead of this one in the same call have committed since. So
 * the per-command cost is a compare on two values already in registers, and the
 * window's shift-and-mask arithmetic is paid once per batch rather than once
 * per order. @see screen_state, risk_gate::open_batch
 *
 * @par What that makes it, and why the batch is the unit
 * A hundred orders in one call are rate limited against each other, which is
 * the case that matters: a loop does not pause between commands to let a window
 * roll.
 *
 * @par Who this may not refuse
 * New liquidity only. A cancel or a reduction is *charged* against the window -
 * it is a real message and it should crowd out new orders - but never refused
 * on account of it, so @c risk_gate::screen_reducing does not call this at all.
 * A throttle that blocks withdrawals is not a throttle, it is a trap: the
 * moment a strategy most needs to pull its quotes is the moment it has been
 * sending the most.
 *
 * @note A fixed window admits up to 2x the limit across a boundary - burst n at
 *       the end of one window, n at the start of the next. Bounded, documented,
 *       and the reason the default window is about a millisecond; a sliding
 *       window would need a division on this path. @see TODO.md #11
 */
[[nodiscard]] inline breach_bits
rate_breach(const screen_state &state) noexcept {
	return bit_if(state.charged >= state.headroom, breach::MESSAGE_RATE);
}

} // namespace exchange::risk::hooks::pre_trade

// Re-exported flat: this type is filed under the hook that owns it, and a
// caller wiring a gate has no business knowing which one that is.
// @see risk_management/fwd.hpp
namespace exchange::risk {
using hooks::pre_trade::rate_limiter;
} // namespace exchange::risk

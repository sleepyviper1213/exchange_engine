#pragma once
// The policy half of the post-trade lane, and the reason it is not in
// risk_limits.
//
// `risk_limits` is copied into every gate and read on every command - it wants
// to be one cache line the hot path already owns. Not one of the numbers below
// is ever read by a pre-trade rule: they are consumed on the dispatcher thread,
// after the fact, by a monitor the gate does not know exists. Putting them in
// `risk_limits` would grow the line the per-command arithmetic reads with data
// that path never touches, which is the wrong trade for the convenience of one
// configuration struct instead of two.
//
// The split also says something true: a deployment can run this lane with
// different thresholds per listing while sharing one `risk_limits`, or run no
// post-trade monitoring at all, and neither is a special case.

#include "risk_management/hooks/detail/fixed_window.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstdint>

namespace exchange::risk::hooks::post_trade {

/**
 * @brief Thresholds for one listing's post-trade rules.
 *
 * @par Defaults are disabled, not permissive
 * Every field defaults to its "off" value, and that is a stronger statement
 * than @c risk_limits' permissive defaults. A pre-trade limit left at its
 * maximum still runs - it just never fires. A post-trade rule left at zero does
 * not run at all, which is what a deployment that has not thought about
 * surveillance should get: a rule nobody sized is a rule that trips at the
 * wrong time, and a breaker that trips at the wrong time gets re-armed
 * reflexively until nobody believes it.
 */
struct post_trade_limits {
	/// @brief The value of every threshold below that means "off".
	static constexpr std::uint32_t DISABLED = 0;

	/// @brief Messages a window must see before a ratio means anything.
	static constexpr std::uint32_t DEFAULT_MESSAGE_FLOOR = 100;

	/// @brief About 1.07 s - the ratio's window. @see ratio_window_log2_ns
	static constexpr unsigned DEFAULT_RATIO_WINDOW_LOG2_NS = 30;

	// --- order-to-trade ratio ---------------------------------------------

	/**
	 * @brief Most messages the account may send per execution, over
	 *        @c ratio_window_log2_ns. Zero disables the rule.
	 *
	 * Whole messages per whole execution: a venue's own OTR cap is quoted this
	 * way (250:1, 500:1) and the granularity of a ratio that trips a breaker
	 * does not repay the fixed-point arithmetic a fractional one would need.
	 */
	std::uint32_t max_messages_per_execution = 0;

	/**
	 * @brief Messages that must accumulate in the window before the ratio is
	 *        judged at all.
	 *
	 * Without a floor the rule is nonsense at the start of every window: one
	 * message and no executions is a ratio of infinity, and a strategy that
	 * quotes once and waits would trip instantly. The floor is what makes
	 * "messages per execution" a statement about behaviour rather than about
	 * the first event to arrive.
	 */
	std::uint32_t min_messages_to_judge = DEFAULT_MESSAGE_FLOOR;

	/**
	 * @brief Base-2 log of the ratio's window in nanoseconds. Default is about
	 *        1.1 s.
	 *
	 * Longer than every other window in this module, and deliberately: an OTR
	 * is a statement about a strategy's *style*, and a millisecond of it is
	 * noise. The cost is the coarse boundary every fixed window has - see
	 * @c fixed_window - which matters less here than anywhere else, because
	 * "roughly this many messages per fill over roughly this long" is the
	 * question. @c fixed_window::MAX_LOG2_NS caps it at about 18 minutes; a
	 * per-session total is @c order_trade_ratio's lifetime counters, not a
	 * wider window.
	 */
	unsigned ratio_window_log2_ns = DEFAULT_RATIO_WINDOW_LOG2_NS;

	// --- fill burst -------------------------------------------------------

	/// @brief Most executions the listing may print in one burst window. Zero
	///        disables the count half of the rule.
	std::uint32_t max_executions_per_window = 0;

	/// @brief Most volume, in lots, the listing may print in one burst window.
	///        Zero disables the volume half. Not implied by the count: one
	///        thousand-lot print and a thousand one-lot prints are different
	///        failures, and a strategy can suffer either.
	volume_t max_volume_per_window = 0;

	/// @brief Base-2 log of the burst window in nanoseconds. Default is about
	///        1.05 ms - short, because "at once" is what a burst means.
	unsigned burst_window_log2_ns = detail::fixed_window::DEFAULT_LOG2_NS;

	/**
	 * @brief Consecutive prints moving the same way that count as the tape
	 *        running. Zero disables.
	 *
	 * A run is a count of *prints*, not a distance in ticks, and it does not
	 * expire with a window - a run is broken by a print in the other direction
	 * and by nothing else. That is the shape of the thing being detected: being
	 * picked off is a sequence, and it is no less a sequence for having taken a
	 * second.
	 */
	std::uint32_t max_adverse_run = 0;

	// --- outcome silence --------------------------------------------------

	/**
	 * @brief Silence from the order return path, in nanoseconds, that means the
	 *        working set can no longer be believed. Zero disables.
	 *
	 * Size it against the venue's acknowledgement latency and not against how
	 * quiet a market can get: this measures the *return path*, so it is silent
	 * because nothing is coming back, not because nothing is trading. It only
	 * ever fires while the gate believes it has orders working, so a strategy
	 * that has gone flat and idle does not trip it.
	 */
	std::uint64_t outcome_timeout_ns = 0;

};

/*
 * The four questions above, asked from outside. Free functions because the type
 * is a set of independently-settable thresholds with nothing to protect between
 * them: a member predicate on public data claims an authority it does not have,
 * since a caller can change a field the moment after asking. Found by
 * argument-dependent lookup, so a call site reads the same minus a dot.
 */

/// @brief Whether the ratio rule is configured.
[[nodiscard]] constexpr bool
has_ratio_limit(const post_trade_limits &limits) noexcept {
	return limits.max_messages_per_execution != post_trade_limits::DISABLED;
}

/// @brief Whether either half of the burst rule is configured.
[[nodiscard]] constexpr bool
has_burst_limit(const post_trade_limits &limits) noexcept {
	return limits.max_executions_per_window != post_trade_limits::DISABLED ||
		   limits.max_volume_per_window != 0;
}

/// @brief Whether the adverse-run rule is configured.
[[nodiscard]] constexpr bool
has_run_limit(const post_trade_limits &limits) noexcept {
	return limits.max_adverse_run != post_trade_limits::DISABLED;
}

/// @brief Whether the silence rule is configured.
[[nodiscard]] constexpr bool
has_silence_limit(const post_trade_limits &limits) noexcept {
	return limits.outcome_timeout_ns != 0;
}

} // namespace exchange::risk::hooks::post_trade
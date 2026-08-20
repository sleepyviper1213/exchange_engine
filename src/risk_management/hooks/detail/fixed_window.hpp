#pragma once
// The counting window every rate-shaped rule in this module measures against,
// and the reason none of them divides.
//
// A window is "how much happened recently", and the textbook spelling of that
// is a sliding window or a token bucket - both of which need `elapsed /
// interval` somewhere, and an integer division is twenty to forty cycles. Every
// trick for avoiding one ends up making the interval a power of two, and once
// it is a power of two the window's index is just `now_ns >> log2`. So that is
// what this is: an epoch, a count, and an AND that throws the count away when
// the epoch has moved on.
//
// It lives in `detail/` because it is arithmetic rather than a rule. A caller
// wiring risk names `rate_limiter` and `order_trade_ratio`; that both of them
// count the same way is this module's business.
//
// @note `rate_limiter` and `circuit_breaker` predate this header and inline the
//       same arithmetic by hand. Adopting it there is mechanical and behaviour
//       preserving, and it is deliberately not part of the change that added
//       this - it would put a rewrite of two hot-path classes inside a change
//       about surveillance. @see TODO.md #11

#include <cstdint>
#include <limits>

namespace exchange::risk::hooks::detail {

/**
 * @brief A count of things that happened in the @c 2^log2_ns nanosecond window
 *        a reading falls in.
 *
 * @par What a fixed window costs, stated honestly
 * It admits up to twice its allowance across a boundary - a full window's worth
 * at the end of one and again at the start of the next - where a sliding window
 * would admit one window's worth in any interval of that length. That is a real
 * difference, and the mitigation is to keep the window *short* relative to what
 * the rule is protecting: a rule whose window is a millisecond has a
 * two-millisecond worst case, which nothing downstream notices. A rule that
 * genuinely wants a long window - an order-to-trade ratio measured over minutes
 * - is buying a coarse boundary in exchange for the division, and says so where
 * it configures the width.
 *
 * @par No rollover bookkeeping
 * @c count never has to be told that time passed. @c -(epoch == epoch_) is
 * all-ones while the stored count still belongs to the caller's window and zero
 * once it does not, so the AND either keeps the count or produces a fresh zero.
 * A window left untouched for an hour reads as empty the moment it is asked,
 * with no branch and nobody having noticed.
 *
 * @par Not atomic, and not thread-safe
 * Every user of this is a single-writer counter owned by one thread - the
 * producer thread for a rate limit, the dispatcher thread for a post-trade
 * rule. Sharing one would need a @c fetch_add per event and would be measuring
 * something nobody asked about; a firm-wide aggregate belongs beside whatever
 * aggregates, with its own atomics. @see TODO.md #11
 */
class fixed_window {
public:
	/// @brief Widest window the shift may name. A shift of 64 or more is
	///        undefined, and anything close is a window longer than a session.
	static constexpr unsigned MAX_LOG2_NS = 40; // ~18 minutes

	/// @brief About 1.05 ms - the width that makes "per window" mean "at once".
	static constexpr unsigned DEFAULT_LOG2_NS = 20;

	/**
	 * @brief A window @c 2^log2_ns nanoseconds wide.
	 * @param log2_ns Base-2 log of the width in nanoseconds, clamped to
	 *        @c MAX_LOG2_NS rather than asserted: a width is configuration, and
	 *        a deployment that names an absurd one should get the widest
	 *        supported window instead of a process that will not start.
	 */
	constexpr explicit fixed_window(unsigned log2_ns = DEFAULT_LOG2_NS) noexcept
		: shift_(log2_ns <= MAX_LOG2_NS ? log2_ns : MAX_LOG2_NS) {}

	/// @brief The width in nanoseconds.
	[[nodiscard]] constexpr std::uint64_t width_ns() const noexcept {
		return std::uint64_t{1} << shift_;
	}

	/// @brief Base-2 log of the width, as configured.
	[[nodiscard]] constexpr unsigned log2_ns() const noexcept { return shift_; }

	/// @brief What has been counted in the window @p now_ns falls in. Pure.
	[[nodiscard]] constexpr std::uint64_t
	count(std::uint64_t now_ns) const noexcept {
		const std::uint64_t epoch = now_ns >> shift_;
		return count_ & -static_cast<std::uint64_t>(epoch == epoch_);
	}

	/**
	 * @brief Add @p n to @p now_ns's window and return the new count.
	 *
	 * Saturates rather than wrapping. A wrap would hand a rule that is already
	 * over its threshold a fresh empty window, which is precisely backwards -
	 * the runaway case is the one that would reach the ceiling.
	 */
	constexpr std::uint64_t add(std::uint64_t now_ns,
								std::uint64_t n) noexcept {
		constexpr std::uint64_t CEILING =
			std::numeric_limits<std::uint64_t>::max();
		const std::uint64_t held = count(now_ns);
		epoch_                   = now_ns >> shift_;
		count_                   = (held > CEILING - n) ? CEILING : held + n;
		return count_;
	}

	/// @brief Forget the current window. A session boundary, or a test.
	constexpr void reset() noexcept {
		epoch_ = 0;
		count_ = 0;
	}

private:
	unsigned shift_;
	/// @brief Which window @c count_ belongs to. Zero is a real epoch - the one
	///        containing t=0 - and that is harmless: @c count_ starts at zero
	///        too, so a fresh window reads as empty either way.
	std::uint64_t epoch_ = 0;
	std::uint64_t count_ = 0;
};

} // namespace exchange::risk::hooks::detail

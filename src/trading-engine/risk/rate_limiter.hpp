#pragma once
// How many messages this producer may send, and how that question is answered
// without a division.

#include "fwd.hpp"

#include <cstdint>
#include <limits>

namespace exchange::engine::risk {

/**
 * @brief A fixed-window message counter: the epoch is a shift of the clock, so
 *        asking costs a shift, a compare and an AND.
 *
 * @par Why not a token bucket
 * A token bucket is the textbook answer and it is the wrong one here, for one
 * reason: refilling means @c elapsed_ns / @c ns_per_token, and an integer
 * division is twenty to forty cycles on a path that is supposed to cost a
 * handful in total. Every trick for avoiding it — a reciprocal multiply, a
 * pre-scaled tick counter — is a way of making the window a power of two, and
 * once the window is a power of two you may as well index by it directly.
 *
 * So the window is @c 2^window_log2_ns nanoseconds and the current window's
 * index is @c now_ns >> window_log2_ns. A shift. Nothing else on the path
 * touches the clock's magnitude at all.
 *
 * @par What that costs, stated honestly
 * A fixed window admits up to @c 2*limit messages across a window boundary —
 * @c limit at the end of one and @c limit at the start of the next — where a
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
 * thread — the same thread that owns the SPSC queue's producer side. Sharing
 * one limiter across threads would need a @c fetch_add per message and would be
 * measuring something nobody asked about. A firm-wide cap belongs one level up,
 * beside the aggregation that can afford it.
 *
 * @par Screening and charging are separate calls
 * @c headroom asks and changes nothing; @c charge commits. The gate needs that
 * split because it screens a whole batch before it knows whether the batch will
 * be accepted downstream — see @c risk_gate on why nothing is committed until
 * the sink has taken the commands.
 */
class rate_limiter {
public:
	/// @brief About 1.05 ms. @see the class note on why short windows matter.
	static constexpr unsigned DEFAULT_WINDOW_LOG2_NS = 20;

	/// @brief Widest window the shift may name — a shift of 64 or more is
	///        undefined, and anything close is a window longer than a session.
	static constexpr unsigned MAX_WINDOW_LOG2_NS = 40; // ~18 minutes

	/**
	 * @brief A limiter admitting @p max_per_window messages per window.
	 * @param max_per_window Allowance. Zero admits nothing.
	 * @param window_log2_ns Base-2 log of the window in nanoseconds.
	 *        @pre at most @c MAX_WINDOW_LOG2_NS.
	 */
	constexpr explicit rate_limiter(
		std::uint32_t max_per_window,
		unsigned window_log2_ns = DEFAULT_WINDOW_LOG2_NS) noexcept
		: limit_(max_per_window),
		  shift_(window_log2_ns <= MAX_WINDOW_LOG2_NS ? window_log2_ns
													  : MAX_WINDOW_LOG2_NS) {}

	/// @brief The allowance per window.
	[[nodiscard]] constexpr std::uint32_t limit() const noexcept {
		return limit_;
	}

	/// @brief The window's width in nanoseconds.
	[[nodiscard]] constexpr std::uint64_t window_ns() const noexcept {
		return std::uint64_t{1} << shift_;
	}

	/**
	 * @brief Messages already charged in the window @p now_ns falls in.
	 *
	 * The branchless half: @c -(epoch == epoch_) is all-ones when the stored
	 * count still belongs to this window and zero when it belongs to a past
	 * one, so the AND either keeps the count or produces a fresh zero. No
	 * branch, and no need to have noticed the rollover beforehand — a limiter
	 * left untouched for an hour reports zero used the moment it is asked.
	 */
	[[nodiscard]] constexpr std::uint32_t
	used(std::uint64_t now_ns) const noexcept {
		const std::uint64_t epoch = now_ns >> shift_;
		return used_ & -static_cast<std::uint32_t>(epoch == epoch_);
	}

	/// @brief How many more messages fit in @p now_ns's window. Pure.
	[[nodiscard]] constexpr std::uint32_t
	headroom(std::uint64_t now_ns) const noexcept { 
		const std::uint32_t spent = used(now_ns);
#ifdef __cpp_lib_saturation_arithmetic
		return std::saturating_sub(limit_, spent);
#endif
		return limit_ < spent ? 0U : limit_ - spent;
	}

	/// @brief Whether @p count more messages would fit. Pure.
	[[nodiscard]] constexpr bool admits(std::uint64_t now_ns,
										std::uint32_t count) const noexcept {
		return count <= headroom(now_ns);
	}

	/**
	 * @brief Charge @p count messages against @p now_ns's window.
	 *
	 * Unchecked: the caller screened with @c headroom and is committing what it
	 * was told fits. Charging past the limit saturates rather than wrapping, so
	 * a caller that skipped the screen throttles itself instead of unlocking a
	 * full window.
	 */
	constexpr void charge(std::uint64_t now_ns, std::uint32_t count) noexcept {
		const std::uint64_t epoch = now_ns >> shift_;
		constexpr std::uint32_t CEILING =
			std::numeric_limits<std::uint32_t>::max();
		used_  = used(now_ns);
		epoch_ = epoch;
		used_  = (used_ > CEILING - count) ? CEILING : used_ + count;
	}

	/// @brief Forget the current window. A session boundary, or a test.
	constexpr void reset() noexcept {
		epoch_ = 0;
		used_  = 0;
	}

private:
	std::uint32_t limit_;
	unsigned shift_;
	/// @brief Which window @c used_ belongs to. Zero is a real epoch — the one
	///        containing t=0 — and that is harmless: @c used_ starts at zero
	///        too, so a fresh limiter reads as unused either way.
	std::uint64_t epoch_ = 0;
	std::uint32_t used_  = 0;
};

} // namespace exchange::engine::risk

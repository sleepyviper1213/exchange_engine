#pragma once
// Where "now" comes from when the session being traded finished months ago.

#include "fwd.hpp"
#include "risk_management/clock.hpp"

#include <cstdint>

namespace exchange::strategy::backtest {

/**
 * @brief The clock a replay runs on: the feed's own timestamps, not the wall.
 *
 * @par Why a backtest may not read a real clock, anywhere
 * Every time-dependent decision in the engine - the gate's rate window, the
 * breaker's breach window, a clocked strategy's schedule - is defined in terms
 * of elapsed nanoseconds. Read those from @c steady_clock during a replay and
 * they measure how fast the *replay* ran: a capture covering ten minutes of
 * market replays in eighty milliseconds, so a rate limit that would never have
 * been reached live is breached on the first frame, and a strategy that
 * requotes every second requotes once for the whole file. The run then measures
 * the machine, and re-running it on a faster one gives different answers.
 *
 * Driving the same clock from @c depth_event::event_time fixes both halves at
 * once. Intervals come out in market time, so the limits fire where they would
 * have fired live; and the run becomes a pure function of the capture, so two
 * runs of the same file on two machines produce identical reports. That
 * determinism is the property a backtest is *for* - without it a result cannot
 * be attributed to a change in the strategy.
 *
 * @c risk::nanosecond_clock exists precisely so this can be substituted; its
 * own documentation names "a replay driving recorded traffic" as the reason the
 * clock is a template parameter rather than a call.
 *
 * @par Monotonicity is enforced, not assumed
 * A venue stamps frames on its own side and a capture can hold an event whose
 * time precedes its predecessor's - a clock adjustment, or two gateways behind
 * one stream. Handing that to a rate limiter would compute a negative interval
 * and open the window permanently, so a regressing stamp is refused and counted
 * instead. @c regressions() being non-zero says the capture's time axis is not
 * clean, which is a fact about the recording worth reporting rather than
 * smoothing over.
 *
 * @note Not thread-safe, and need not be: a backtest is single-threaded by
 *       construction. @see session
 */
class feed_clock {
public:
	/**
	 * @brief Move time forward to @p event_ns, nanoseconds since the epoch.
	 *
	 * @param event_ns The frame's stamp. Zero is "unstamped" - some venues omit
	 *        the field on some payloads (Binance's REST depth carries no event
	 *        time at all) - and leaves the clock where it was rather than
	 *        rewinding it to the epoch.
	 */
	void advance_to(std::uint64_t event_ns) noexcept {
		if (event_ns == 0) return;
		if (!stamped_) {
			first_ns_ = event_ns;
			stamped_  = true;
		}
		if (event_ns < now_ns_) {
			++regressions_;
			return;
		}
		now_ns_ = event_ns;
	}

	/// @brief The current market time, as engine-monotonic time.
	///
	/// A venue stamp is a wall-clock instant from another machine; what makes
	/// it usable as monotonic time is @c advance_to refusing to move backwards,
	/// and that refusal is why this can honestly hand back a @c monotonic_time.
	[[nodiscard]] risk::monotonic_time now() const noexcept {
		return risk::monotonic_time{risk::monotonic_clock::duration{
			static_cast<risk::monotonic_clock::rep>(now_ns_)}};
	}

	/// @brief Deprecated: the raw reading. @see now
	[[nodiscard]] std::uint64_t now_ns() const noexcept { return now_ns_; }

	/// @brief The first stamp seen, or 0 if none has been.
	[[nodiscard]] std::uint64_t first_ns() const noexcept { return first_ns_; }

	/// @brief Market time the run has covered so far.
	[[nodiscard]] std::uint64_t elapsed_ns() const noexcept {
		return stamped_ ? now_ns_ - first_ns_ : 0;
	}

	/// @brief Stamps that would have moved time backwards, and were refused.
	///        @see the class note.
	[[nodiscard]] std::uint64_t regressions() const noexcept {
		return regressions_;
	}

	/// @brief Whether any event has carried a usable stamp.
	[[nodiscard]] bool stamped() const noexcept { return stamped_; }

private:
	std::uint64_t now_ns_      = 0;
	std::uint64_t first_ns_    = 0;
	std::uint64_t regressions_ = 0;
	bool stamped_              = false;
};

/**
 * @brief A non-owning reader of a @c feed_clock, satisfying
 *        @c risk::nanosecond_clock.
 *
 * The gate holds its clock *by value* (@c [[no_unique_address]]), which is
 * right for @c steady_nanos - a stateless type that costs nothing to copy - and
 * wrong for a clock somebody else advances. One pointer restores the reference
 * semantics without giving the gate a template parameter it would have to know
 * about.
 */
class clock_view {
public:
	explicit clock_view(const feed_clock &clock) noexcept : clock_(&clock) {}

	[[nodiscard]] risk::monotonic_time now() const noexcept {
		return clock_->now();
	}

	/// @brief Deprecated: the raw reading. @see now
	[[nodiscard]] std::uint64_t now_ns() const noexcept {
		return clock_->now_ns();
	}

private:
	const feed_clock *clock_;
};

static_assert(risk::nanosecond_clock<clock_view>,
			  "the gate must accept the replay clock, or a backtest measures "
			  "how fast it replayed rather than what the market did");

} // namespace exchange::strategy::backtest

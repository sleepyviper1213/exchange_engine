#pragma once
// The emergency stop: one byte anybody may read, two parties may write, and no
// lock anywhere.

#include "core/util/enum_string.hpp"
#include "fwd.hpp"

#include <atomic>
#include <cstdint>

namespace exchange::risk {

#define RISK_TRADING_STATE_LIST(X)                                             \
	X(NORMAL, "every command passes the ordinary checks")                      \
	X(CANCEL_ONLY, "risk-reducing commands only; no new liquidity")            \
	X(HALTED, "nothing passes, cancels included")

/**
 * @brief What the gate is currently willing to let through.
 *
 * @par Why @c CANCEL_ONLY is the interesting state and @c HALTED is not
 * A kill switch that blocks everything also blocks the *withdrawals* — it
 * freezes a malfunctioning strategy's orders in the book and leaves them there
 * to be filled by whoever noticed. That is the wrong emergency behaviour, and
 * it is why every real venue's halt still accepts cancels. @c CANCEL_ONLY is
 * therefore what the automatic trip selects: stop adding risk, keep the ability
 * to shed it.
 *
 * @c HALTED exists for the narrower case where the strategy itself is not
 * trusted to name the right orders — a bad deploy sending cancels for ids it
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
	X(BREACH_RATE, "too many refusals in one window — a looping strategy")     \
	X(LOSS_LIMIT, "realised plus unrealised loss passed its floor")

/**
 * @brief Why the breaker last left @c NORMAL.
 *
 * The state says trading stopped; this says what to do about it, and they are
 * different questions. A @c BREACH_RATE trip means a strategy is malfunctioning
 * and someone should read its logs before re-arming. A @c LOSS_LIMIT trip means
 * the strategy is working exactly as written and losing money, which is a
 * decision for a human, not a bug. Re-arming blindly is the wrong response to
 * both, but for opposite reasons — so the cause is recorded rather than left to
 * be inferred from whatever else happened to be on screen.
 */
enum class trip_cause : std::uint8_t {
	EXCHANGE_ENUM_VALUES(RISK_TRIP_CAUSE_LIST)
};

EXCHANGE_ENUM_NAME(trip_cause, to_string, RISK_TRIP_CAUSE_LIST)

EXCHANGE_ENUM_LABEL_ONLY(trip_cause, describe, RISK_TRIP_CAUSE_LIST)

/**
 * @brief The kill switch, tripped by an operator or by the gate itself.
 *
 * @par What the automatic trip is actually detecting
 * Not a bad market — a bad *strategy*. A limit breach is ordinary: a strategy
 * sizes an order against a position that moved, the gate refuses it, the
 * strategy carries on. A strategy breaching over and over inside a millisecond
 * is not sizing anything; it is looping. Counting breaches per window and
 * cutting the line at a threshold catches exactly that failure and almost
 * nothing else, which is what you want from something that stops trading.
 *
 * The counter is the same fixed-window shift @c rate_limiter uses, for the same
 * reason: an epoch is @c now_ns >> shift and a rollover is an AND. @see
 * rate_limiter
 *
 * @par Threads and ordering
 * The state is written by two parties — the gate's own thread when it trips
 * automatically, and an operator's thread when somebody hits the switch — and
 * read by everyone. So it is a real @c std::atomic, unlike the single-writer
 * counters in @c position_book.
 *
 * Both accesses are relaxed, and that is the whole requirement rather than a
 * shortcut. Relaxed is too weak when a flag publishes *something else*: the
 * classic `write the buffer, release the ready flag` needs the reader to see
 * the buffer once it sees the flag. Nothing is published here. The state is the
 * entire message, it fits in one byte, and what a reader needs is that the
 * store becomes visible in bounded time — which cache coherence guarantees
 * without any fence, on every architecture this builds for. An acquire load on
 * the hot path would buy a guarantee about data that does not exist.
 *
 * An operator's @c arm racing the gate's automatic @c trip can be lost, and
 * that is the correct outcome rather than a hole: if the strategy is still
 * looping it trips again on the next breach, and if it is not, the re-arm
 * sticks.
 *
 * @par What the breach counter is *not*
 * It is deliberately not atomic. Only the gate's thread counts breaches, so it
 * is a single-writer counter like the position ones; a second gate wanting to
 * feed the same breaker would need that changed, and would be better served by
 * a breaker each and an aggregator above them.
 */
class circuit_breaker {
public:
	/// @brief A breaker that never trips itself. @see rate_limiter for the
	///        window encoding.
	static constexpr std::uint32_t NO_AUTO_TRIP = 0;

	/// @brief About 1.05 ms — short enough that "breaches per window" means
	///        "looping" rather than "had a bad afternoon".
	static constexpr unsigned DEFAULT_WINDOW_LOG2_NS = 20;

	/**
	 * @param breaches_to_trip Breaches within one window that trip the breaker
	 *        to @c CANCEL_ONLY, or @c NO_AUTO_TRIP for manual operation only.
	 * @param window_log2_ns Base-2 log of the counting window in nanoseconds.
	 */
	explicit circuit_breaker(
		std::uint32_t breaches_to_trip = NO_AUTO_TRIP,
		unsigned window_log2_ns        = DEFAULT_WINDOW_LOG2_NS) noexcept;

	/// @brief The current state. Relaxed — see the class note.
	[[nodiscard]] trading_state state() const noexcept;

	/// @brief Whether new liquidity may be sent.
	[[nodiscard]] bool passes_new_orders() const noexcept;

	/// @brief Whether risk-reducing commands may be sent. True in every state
	///        but @c HALTED.
	[[nodiscard]] bool passes_cancels() const noexcept;

	/// @brief Move to @p to, recording @p why. An operator action by default;
	///        also how the automatic trips record themselves.
	void trip(trading_state to, trip_cause why = trip_cause::OPERATOR) noexcept;

	/// @brief Back to @c NORMAL. Does not clear the breach counter — a re-arm
	///        into a still-looping strategy should trip again immediately, not
	///        start it a fresh allowance — and does not clear @c cause(), which
	///        is history rather than current state.
	void arm() noexcept;

	/// @brief Why the breaker last tripped, or @c NONE if it never has.
	[[nodiscard]] trip_cause cause() const noexcept;

	/**
	 * @brief Count one refused command, and trip if that is the last straw.
	 * @param now_ns Monotonic nanoseconds, from the same clock the gate uses.
	 * @return @c true if this call is what tripped the breaker.
	 */
	bool record_breach(std::uint64_t now_ns) noexcept;

	/// @brief Breaches counted in the window @p now_ns falls in.
	[[nodiscard]] std::uint32_t breaches(std::uint64_t now_ns) const noexcept;

	/// @brief How many times this breaker has left @c NORMAL since construction
	///        — the number an operator looks at first.
	[[nodiscard]] std::uint64_t trips() const noexcept;

	/// @brief The auto-trip threshold, or @c NO_AUTO_TRIP.
	[[nodiscard]] std::uint32_t threshold() const noexcept;

private:
	std::atomic<trading_state> state_{trading_state::NORMAL};
	// Written beside state_ and read independently of it, so the two are not a
	// consistent pair: a reader can catch a new state against the previous
	// cause. Nothing acts on the combination — the state gates commands, the
	// cause is for a human — so pairing them would buy nothing for the
	// synchronisation it would cost on the trip path.
	std::atomic<trip_cause> cause_{trip_cause::NONE};
	std::uint32_t threshold_;
	unsigned shift_;
	std::uint64_t epoch_    = 0;
	std::uint32_t breaches_ = 0;
	std::uint64_t trips_    = 0;

	static_assert(std::atomic<trading_state>::is_always_lock_free,
				  "a kill switch that takes a lock is not a kill switch");
};

} // namespace exchange::risk

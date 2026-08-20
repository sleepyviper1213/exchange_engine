#pragma once
// Outcome Silence: exposure the process believes in and has heard nothing
// about.
//
// The failure `feedback.hpp` warns about, made visible. A gate's limits are
// measured against a working set that only ever shrinks when an outcome comes
// back, so a return path that dies leaves the gate ratcheting closed over a
// session: exposure that never retires, a position that never moves, and a
// strategy that gradually stops being able to quote. Every individual reading
// is plausible. The failure is silent, gradual, and looks exactly like a limit
// somebody set too tight.
//
// It is *not* the heartbeat, and the distinction is the reason this exists.
// `system/heartbeat.hpp` watches market data - prints and diffs arriving from
// the venue - and it is entirely possible for that to be healthy while the
// order return path is dead: the multicast feed is a different session from the
// order session at every venue worth naming, and a strategy that can see the
// market perfectly while its own acknowledgements have stopped is in a worse
// position than one that has gone blind, because it will keep quoting into a
// book it cannot withdraw from.

#include "risk_management/hooks/post_trade/fwd.hpp"
#include "risk_management/hooks/post_trade/limits.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)

#include <cstdint>

namespace exchange::risk::hooks::post_trade {

/**
 * @brief Whether @p working lots of exposure have gone @p silence_ns without a
 *        word.
 *
 * @param silence_ns Nanoseconds since the last outcome.
 * @param working Orders the gate believes are still out there.
 * @param timeout_ns The silence that means the return path is gone. Zero
 *        disables and returns @c false.
 *
 * @note Both conditions, and the conjunction is the rule. Silence on its own is
 *       a strategy that has gone flat and idle, which is not a fault and must
 *       not stop trading. Working orders on their own are the ordinary case. It
 *       is only the pair - we think we are exposed, and nobody will confirm it
 * - that says the process's view of its own risk has stopped being evidence.
 */
[[nodiscard]] constexpr bool
is_silent_with_exposure(std::uint64_t silence_ns, std::uint32_t working,
						std::uint64_t timeout_ns) noexcept {
	return timeout_ns != 0 && working > 0 && silence_ns > timeout_ns;
}

/**
 * @brief Trips the breaker when the gate has orders working and the order
 *        return path has gone quiet.
 *
 * @par Polled, not fired - and this one could not be anything else
 * The other two rules in this lane are driven by the events they count, so they
 * evaluate themselves as those arrive. This rule's whole subject is the
 * *absence* of events, and an absence delivers no callback. So it is polled
 * from whatever loop already runs - the dispatcher's pump, a console tick - and
 * a monitor nobody polls never trips. That is a property worth stating plainly
 * rather than leaving to be discovered, and it is the same contract @c
 * heartbeat_monitor carries for the same reason.
 *
 * @par Why the working count is a parameter
 * Because the answer belongs to the gate and the question belongs here. A rule
 * that held a gate pointer would point an edge from this lane at
 * @c risk_gate - a template, instantiated in its consumer's translation unit -
 * for one @c std::uint32_t. @c feedback_router already holds both halves and is
 * the thing that drives the poll, so it passes the reading in. @see
 * post_trade_monitor::poll
 *
 * @par What it deliberately does not measure
 * The age of any *particular* order. "Order 41 has rested for a minute with no
 * outcome" is the sharper rule and it needs a walk over @c working_ledger,
 * which has no iteration - it is an open-addressed probe table built for one
 * lookup per command, and giving it iteration is a change to a hot-path
 * structure for the benefit of a rule that runs on another thread. So this
 * measures the aggregate: exposure outstanding, and nothing at all coming back.
 * That catches the return path dying, which is the failure that takes the whole
 * session with it, and misses the single order that got lost on its own. @see
 * TODO.md #11
 *
 * @par Threading
 * One of these per listing, owned by @c post_trade_monitor, on the dispatcher
 * thread - the same thread that receives the outcomes it is timing. Plain
 * counters, single writer, and the decision leaves the thread through
 * @c circuit_breaker.
 */
class outcome_silence {
public:
	/// @brief A rule that never trips - the disabled configuration, so a
	///        deployment that does not want it spells that rather than omitting
	///        the object and changing its wiring.
	static constexpr std::uint64_t NO_TIMEOUT = 0;

	/**
	 * @brief Watch @p breaker, tripping it after @p timeout_ns of silence.
	 *
	 * @param breaker The shared kill switch. Must outlive the rule.
	 * @param limits The policy. One field is read here - the timeout - and the
	 *        whole object is taken anyway, so all three rules in this lane are
	 *        built the same way.
	 * @param now_ns Counts as the first outcome, so a monitor built before the
	 *        session has sent anything gets its whole timeout rather than
	 *        tripping on silence that predates it. It cannot trip before the
	 *        first order anyway - there is no exposure to be silent about - but
	 *        seeding it keeps @c silence_ns honest from the first reading.
	 */
	RISK_MANAGEMENT_EXPORT outcome_silence(system::circuit_breaker &breaker,
										   const post_trade_limits &limits,
										   std::uint64_t now_ns) noexcept;

	/// @brief An outcome arrived at @p now_ns. Any outcome counts: this is
	///        evidence the return path is alive, not a measurement of what it
	///        said.
	RISK_MANAGEMENT_EXPORT void beat(std::uint64_t now_ns) noexcept;

	/**
	 * @brief Check for silence as of @p now_ns against @p working orders, and
	 *        trip if there is too much of it.
	 * @return Whether *this call* is what tripped the breaker.
	 *
	 * @note Does not trip a breaker that is already open, so one outage
	 *       produces one trip. It will trip again if an operator re-arms while
	 *       the path is still silent, which matches @c circuit_breaker's own
	 *       contract: a re-arm into a condition that still holds does not buy a
	 *       fresh allowance.
	 */
	RISK_MANAGEMENT_EXPORT bool poll(std::uint64_t now_ns,
									 std::uint32_t working) noexcept;

	/// @brief Whether @p working orders are past the timeout as of @p now_ns.
	///        Always @c false when disabled or when nothing is working.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	is_silent(std::uint64_t now_ns, std::uint32_t working) const noexcept;

	/// @brief Nanoseconds between the last outcome and @p now_ns.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	silence_ns(std::uint64_t now_ns) const noexcept;

	/// @brief The reading the last outcome was stamped with.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	last_outcome_ns() const noexcept;

	/// @brief The silence this rule treats as a dead return path, or
	///        @c NO_TIMEOUT.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	timeout_ns() const noexcept;

	/// @brief Outcomes seen since construction.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	outcomes() const noexcept;

	/// @brief Times this rule has tripped the breaker.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t trips() const noexcept;

private:
	system::circuit_breaker *breaker_;
	std::uint64_t timeout_ns_;
	std::uint64_t last_outcome_ns_;
	std::uint64_t outcomes_ = 0;
	std::uint64_t trips_    = 0;
};

} // namespace exchange::risk::hooks::post_trade
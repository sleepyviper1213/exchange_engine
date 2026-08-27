#pragma once
// Order-to-Trade Ratio: how much talking the strategy did per thing it actually
// did, and what counts as talking.
//
// The failure no pre-trade rule can see. A strategy that places and cancels a
// thousand times for every fill breaks no limit on any one of those messages -
// each is small, correctly priced, inside every position bound - and the rate
// limiter is satisfied because the messages are spread out. What is wrong is
// the *ratio*, and a ratio is a property of a sequence.
//
// It is also the one rule in this lane that a venue is likely to be measuring
// too. An OTR cap is a published number at most European venues and a
// contractual one at many others, enforced with fees and then with
// disconnection - and a disconnection takes the cancels with it, which is the
// same argument that puts a rate limiter in front of the gateway. Measuring it
// ourselves is how the breaker trips before the venue's does.

// The macro is used on every out-of-line member below, so it is included here
// rather than inherited from a forward-declaration header.
#include "fwd.hpp"
#include "limits.hpp"
#include "order_book/outcome.hpp"
#include "risk_management/hooks/detail/fixed_window.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)

#include <cstdint>

namespace exchange::risk::hooks::post_trade {

/**
 * @brief Whether @p messages per @p executions is over @p max_per_execution.
 *
 * @param messages Messages counted in the window.
 * @param executions Executions counted in the same window.
 * @param max_per_execution The cap, as whole messages per whole execution.
 *        Zero disables, and returns @c false.
 * @param floor Messages that must have accumulated before the ratio is judged.
 *        Below it the answer is @c false whatever the ratio is.
 *
 * @par Why this is a multiply and not a divide
 * @c messages/executions is the natural spelling and it needs a guard for
 * @c executions==0 and a division on top. @c messages > executions*cap needs
 * neither: with no executions it reduces to @c messages>0, which the floor is
 * already the correct answer to, and the multiply is three cycles rather than
 * twenty. This rule is not on the hot path and would survive the division - the
 * reason to write it this way is that the multiplied form is also the one with
 * no special case in it.
 *
 * @note @c executions*max_per_execution is a 64-bit product of a windowed count
 *       and a 32-bit cap. Overflowing it needs about @c 2^32 executions inside
 *       one window, which is more prints than the listing will see in a
 *       session; a saturating window keeps the counts themselves finite. @see
 *       fixed_window::add
 */
[[nodiscard]] constexpr bool is_over_ratio(std::uint64_t messages,
										   std::uint64_t executions,
										   std::uint32_t max_per_execution,
										   std::uint32_t floor) noexcept {
	return max_per_execution != 0 && messages >= floor &&
		   messages > executions * max_per_execution;
}

/**
 * @brief Whether @p record is a message the venue had to process.
 *
 * @par The counting policy, which is the whole difficulty of this rule
 * An OTR's numerator is messages *received by the venue*, and the lane's only
 * evidence of those is the outcome stream. It is close but not one-for-one, and
 * the three places it differs all matter:
 *
 * - A @c FILL is not a message. It is the venue telling us something, which is
 *   the opposite direction, and counting it would let a strategy improve its
 *   ratio by trading badly.
 * - A @c CANCELLED carrying @c TIME_IN_FORCE is the book withdrawing an IOC
 *   remainder on its own initiative. Nobody sent a cancel, so nothing is
 *   counted - a strategy that quotes exclusively in IOCs would otherwise be
 *   charged twice for every order it sent.
 * - A @c REJECTED and a @c CANCEL_REJECTED both *are* counted. The venue
 *   received the message, parsed it and answered it; that it answered "no" is
 *   not a discount, and this is exactly how a venue's own counter works.
 *
 * @note Commands the *gate* refused never appear here at all, because they
 *       never reached the book and so produced no outcome. That is the right
 *       answer for a rule that exists to predict the venue's own measurement,
 *       and it is why this counts outcomes rather than reading the gate's
 *       @c passed() counter: the venue cannot charge us for a message we did
 *       not send.
 */
[[nodiscard]] constexpr bool
is_venue_message(const engine::order_outcome &record) noexcept {
	switch (record.type) {
	case engine::OutcomeType::ACCEPTED:
	case engine::OutcomeType::REJECTED:
	case engine::OutcomeType::CANCEL_REJECTED: return true;
	case engine::OutcomeType::CANCELLED:
		return record.reason != engine::reject_reason::TIME_IN_FORCE;
	case engine::OutcomeType::FILL: return false;
	}
	return false;
}

/**
 * @brief Counts messages and executions in a window and trips the breaker when
 *        the ratio between them says the strategy is quoting rather than
 *        trading.
 *
 * @par Why it trips rather than refuses
 * The same reason the drawdown breaker does, and it is worth spelling out
 * because this rule is the one that most looks like it could refuse. A high
 * ratio is not the fault of the message that pushed it over - that message is
 * indistinguishable from the thousand before it - so refusing it while
 * accepting the next identical one would leave the ratio exactly where it was
 * and produce a reject the strategy cannot act on. Cutting to @c CANCEL_ONLY
 * stops the quoting, which is the behaviour, and leaves the withdrawals open,
 * which is what a strategy in that state needs to do.
 *
 * @par Both counters, one window
 * Messages and executions are counted in the same @c fixed_window width so the
 * ratio is between two numbers describing the same interval. They are two
 * windows rather than one because they roll independently of each other only in
 * the sense that either may be empty - and a shared epoch would make an
 * execution reset the message count, which would be a rule that congratulates a
 * strategy for the fill it just got.
 *
 * @par Threading
 * One of these per listing, owned by @c post_trade_monitor, on the dispatcher
 * thread. The counters are plain because they are single-writer, like every
 * other counter in this module; the decision leaves the thread through
 * @c circuit_breaker, which is atomic precisely so that it can.
 */
class order_trade_ratio {
public:
	/// @brief A rule that never trips - the disabled configuration.
	static constexpr std::uint32_t NO_LIMIT = 0;

	/**
	 * @brief Watch @p breaker under @p limits.
	 *
	 * @param breaker The shared kill switch. Must outlive the rule.
	 * @param limits The policy. Three of its fields are read here - the cap,
	 *        the floor and the window; the rest belong to the other two rules.
	 *
	 * @note The whole policy object rather than the three numbers, the way
	 *       @c size_breaches takes a whole @c risk_limits. A constructor with
	 *       three adjacent integers is one a caller can transpose into a rule
	 *       that never fires, and clang-tidy flags exactly that shape.
	 */
	RISK_MANAGEMENT_EXPORT
	order_trade_ratio(system::circuit_breaker &breaker,
					  const post_trade_limits &limits) noexcept;

	/**
	 * @brief Count one message the venue processed, at @p now.
	 * @return Whether *this call* is what tripped the breaker.
	 *
	 * Evaluated here rather than in a @c poll because the ratio can only change
	 * when something arrives, and a message is the only arrival that can push
	 * it *up*. An execution moves it down, so recording one never trips and
	 * never needs to check.
	 */
	RISK_MANAGEMENT_EXPORT bool
	record_message(core::chrono::monotonic_time now) noexcept;

	/// @brief Count one execution, at @p now. Never trips - see
	///        @c record_message.
	RISK_MANAGEMENT_EXPORT void
	record_execution(core::chrono::monotonic_time now) noexcept;

	/// @brief Whether the ratio is over its cap as of @p now. Pure, and
	///        @c false whenever the rule is disabled or under its floor.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	is_breaching(core::chrono::monotonic_time now) const noexcept;

	/// @brief Messages counted in @p now's window.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	messages(core::chrono::monotonic_time now) const noexcept;

	/// @brief Executions counted in @p now's window.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	executions(core::chrono::monotonic_time now) const noexcept;

	/// @brief Messages counted since construction - the session view, and the
	///        one an operator compares against the venue's own invoice.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	total_messages() const noexcept;

	/// @brief Executions counted since construction.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	total_executions() const noexcept;

	/// @brief The cap, or @c NO_LIMIT.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint32_t
	threshold() const noexcept;

	/// @brief The window's width in nanoseconds.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	window_ns() const noexcept;

	/// @brief Times this rule has tripped the breaker.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t trips() const noexcept;

private:
	system::circuit_breaker *breaker_;
	std::uint32_t threshold_;
	std::uint32_t floor_;
	detail::fixed_window messages_;
	detail::fixed_window executions_;
	std::uint64_t total_messages_   = 0;
	std::uint64_t total_executions_ = 0;
	std::uint64_t trips_            = 0;
};

} // namespace exchange::risk::hooks::post_trade
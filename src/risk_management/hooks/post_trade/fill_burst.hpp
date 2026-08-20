#pragma once
// Fill Burst: being filled faster than anybody meant to be, and the tape
// running one way through our prints.
//
// Two failures that look identical to every pre-trade rule and to each other on
// paper, and are completely different in what they mean.
//
// A *burst* is mechanical. A quote left up while the market moved, or a size
// left on both sides of a name that just got interesting, and suddenly a
// hundred prints land in a millisecond against orders that each passed every
// limit at submission. Nothing was mispriced when it was sent; the market
// arrived. The only thing that can notice is a counter on the return path.
//
// A *run* is directional. Print after print marching one way through the levels
// we are resting on, which is the shape of adverse selection: not "we traded a
// lot" but "we traded a lot into something". A burst can be a windfall; a run
// almost never is.
//
// So they are one file - one rule asked in two units, the way the size checks
// are - and two trip causes, because the operator's next action differs.

#include "core/util/enum_string.hpp"
#include "risk_management/hooks/detail/fixed_window.hpp"
#include "risk_management/hooks/post_trade/fwd.hpp" // IWYU pragma: export
#include "risk_management/hooks/post_trade/limits.hpp"
#include "risk_management/hooks/system/circuit_breaker.hpp"
#include "risk_management_export.hpp" // RISK_MANAGEMENT_EXPORT (generated)
#include "trading-engine/order_book/trade.hpp"
#include "trading-engine/orders/types.hpp"

#include <cstdint>

namespace exchange::risk::hooks::post_trade {

#define RISK_TAPE_DIRECTION_LIST(X)                                            \
	X(UNKNOWN, "the tape has not moved yet, or the last print was flat")       \
	X(UP, "each print above the one before it")                                \
	X(DOWN, "each print below the one before it")

/**
 * @brief Which way the tape is going, as of the last print that moved it.
 *
 * @c UNKNOWN is load-bearing rather than a null value. Before the second print
 * there is no direction to have - one price is not a move - and a flat print
 * does not create one either. Folding both into @c UP would credit a run to a
 * market that never went anywhere, which is the one reading that would make the
 * run counter fire on a quiet name.
 */
enum class tape_direction : std::uint8_t {
	EXCHANGE_ENUM_VALUES(RISK_TAPE_DIRECTION_LIST)
};

EXCHANGE_ENUM_NAME(tape_direction, to_string, RISK_TAPE_DIRECTION_LIST)

EXCHANGE_ENUM_LABEL_ONLY(tape_direction, describe, RISK_TAPE_DIRECTION_LIST)

#undef RISK_TAPE_DIRECTION_LIST

/**
 * @brief Whether @p executions or @p volume is over its cap for one window.
 *
 * @param executions Prints counted in the window.
 * @param volume Lots printed in the same window.
 * @param max_executions The count cap. Zero disables that half.
 * @param max_volume The volume cap, in lots. Zero disables that half.
 *
 * @note Two caps and one answer, because the breaker has one state to move to
 *       and the diagnosis is the same either way: too much happened at once.
 *       Which half fired is readable from the counters. Both are @c > rather
 *       than @c >=, so a cap is the last admissible value - the same convention
 *       as @c through_floor.
 */
[[nodiscard]] constexpr bool is_over_burst(std::uint64_t executions,
										   std::uint64_t volume,
										   std::uint32_t max_executions,
										   std::uint64_t max_volume) noexcept {
	return (max_executions != 0 && executions > max_executions) ||
		   (max_volume != 0 && volume > max_volume);
}

/// @brief Whether @p run consecutive same-direction moves is over @p max_run.
///        Zero disables.
[[nodiscard]] constexpr bool is_over_run(std::uint32_t run,
										 std::uint32_t max_run) noexcept {
	return max_run != 0 && run > max_run;
}

/**
 * @brief Counts what printed in a window, tracks which way the tape is going,
 *        and trips the breaker when either says the strategy is being run over.
 *
 * @par What it can see, and the limitation that shapes the whole rule
 * An @c engine::trade carries two order ids, a price and a volume, and no side.
 * Which side of a print was ours - and therefore whether a rising tape is good
 * news or a disaster - is a question for the gate's @c working_ledger, and
 * reaching into that from here would either duplicate the ledger or point an
 * edge from this lane at the gate that owns it. Neither is worth it for a rule
 * that trips a breaker a human then reads.
 *
 * So the run counter measures *the tape*, through prints this listing was part
 * of, and does not claim to know which side we were on. In the case that
 * matters it does not need to: a strategy resting on both sides is on the wrong
 * side of any sustained run by construction, and a strategy resting on one side
 * has a position whose sign the operator can read off @c position_book in the
 * same breath as the trip cause. The rule that genuinely needs the side is
 * per-order adverse-fill attribution, and that belongs wherever the ledger is.
 * @see post_trade/fwd.hpp
 *
 * @par Why the run has no window
 * Every other counter here forgets. A run does not, because it is broken by
 * evidence rather than by time: a print in the other direction ends it and
 * nothing else does. A run of forty prints that took a second is still forty
 * prints one way, and a window would have thrown away exactly the slow version
 * of the failure that is hardest to notice by eye.
 *
 * A flat print - the same price twice - neither extends nor breaks a run. It is
 * not a move, so it says nothing about direction, and treating it as a break
 * would let a strategy quoting inside a tight market erase every run it was in.
 *
 * @par Threading
 * One of these per listing, owned by @c post_trade_monitor, on the dispatcher
 * thread. Plain counters, single writer, and the decision leaves the thread
 * through @c circuit_breaker.
 */
class fill_burst {
public:
	/// @brief A rule that never trips - the disabled configuration.
	static constexpr std::uint32_t NO_LIMIT = 0;

	/**
	 * @brief Watch @p breaker for bursts and runs, under @p limits.
	 *
	 * @param breaker The shared kill switch. Must outlive the rule.
	 * @param limits The policy. Four of its fields are read here - two burst
	 *        caps, the run cap and the window.
	 *
	 * @note The whole policy object rather than the four numbers: two of them
	 *       are @c std::uint32_t caps meaning completely different things, so a
	 *       transposition would compile into a rule that fires on the wrong
	 *       evidence. @c auto_trip_after in the gate fixture exists for the
	 *       same reason.
	 *
	 * @note A negative @c max_volume_per_window reads as disabled rather than
	 *       asserting. It is configuration, and the permissive reading is the
	 *       one that does not stop a process from starting.
	 */
	RISK_MANAGEMENT_EXPORT
	fill_burst(system::circuit_breaker &breaker,
			   const post_trade_limits &limits) noexcept;

	/**
	 * @brief Record @p execution, as of @p now_ns.
	 * @return Whether *this call* is what tripped the breaker.
	 * @pre @p execution carries positive volume - a print of nothing is not a
	 *      print, and the book does not publish one.
	 *
	 * Burst is checked before the run: both would select @c CANCEL_ONLY, only
	 * one trip is recorded, and "too much at once" is the reading that wants
	 * looking at first when both are true.
	 *
	 * @note The whole trade rather than its price and volume as two integers.
	 *       Both are numbers a caller could transpose, and the caller already
	 *       has the record - the ids on it are simply not something this rule
	 *       can use. @see post_trade/fwd.hpp
	 */
	RISK_MANAGEMENT_EXPORT bool record(std::uint64_t now_ns,
									   const engine::trade &execution) noexcept;

	/// @brief Whether either cap is exceeded as of @p now_ns. Pure.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool
	is_bursting(std::uint64_t now_ns) const noexcept;

	/// @brief Whether the run is over its cap. Pure, and windowless - see the
	///        class note.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT bool is_running() const noexcept;

	/// @brief Prints counted in @p now_ns's window.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	executions(std::uint64_t now_ns) const noexcept;

	/// @brief Lots printed in @p now_ns's window.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	volume(std::uint64_t now_ns) const noexcept;

	/// @brief Consecutive moves the tape has made in @c direction().
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint32_t run() const noexcept;

	/// @brief Which way the tape is going, or @c UNKNOWN before it has moved.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT tape_direction
	direction() const noexcept;

	/// @brief The last price seen, in ticks. Zero before the first print.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT price_t last_price() const noexcept;

	/// @brief Prints seen since construction.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	total_executions() const noexcept;

	/// @brief Lots printed since construction.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	total_volume() const noexcept;

	/// @brief The window's width in nanoseconds.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t
	window_ns() const noexcept;

	/// @brief Times this rule has tripped the breaker.
	[[nodiscard]] RISK_MANAGEMENT_EXPORT std::uint64_t trips() const noexcept;

private:
	/// @brief Fold @p price into the run counter. @return the new run length.
	std::uint32_t extend_run(price_t price) noexcept;

	system::circuit_breaker *breaker_;
	std::uint32_t max_executions_;
	std::uint64_t max_volume_;
	std::uint32_t max_run_;
	detail::fixed_window executions_;
	detail::fixed_window volume_;

	// Zero is not a price the book prints, so it doubles as "no print yet" -
	// the same convention risk_gate uses for a reference price it has not been
	// given. A first print therefore establishes a price without establishing a
	// direction, which is correct: one print is not a move.
	price_t last_price_        = 0;
	tape_direction direction_  = tape_direction::UNKNOWN;
	std::uint32_t run_         = 0;
	std::uint64_t total_execs_ = 0;
	std::uint64_t total_lots_  = 0;
	std::uint64_t trips_       = 0;
};

} // namespace exchange::risk::hooks::post_trade
#pragma once
// Checking the fill model's claims against the venue's own tape.
//
// The backtest infers passive fills from depth. The tape says what actually
// traded. They are separate subscriptions with nothing relating them but their
// timestamps - a print carries no update id - so the comparison is necessarily
// a match within a window rather than a join on a key. @see capture.hpp, which
// says the same thing about recording the two.

#include "fwd.hpp"
#include "market_data/trade_feed.hpp" // trade_handler - asserted below
#include "market_data/trade_print.hpp"
#include "modelled_fill.hpp"
#include "orders/types.hpp"
#include "strategy_export.hpp" // STRATEGY_EXPORT (generated)
#include "symbol/symbol_spec.hpp"
#include "tape_audit_report.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace exchange::strategy::backtest {

/**
 * @brief Matches modelled passive fills against the prints that would justify
 *        them.
 *
 * @par The window, and why there has to be one
 * A diff feed publishes on a timer - 100ms on Binance's default depth stream -
 * so a fill inferred from the frame stamped @c T actually happened somewhere in
 * the interval that frame summarises. Demanding an exact timestamp match would
 * reject essentially every true fill and report the model as fabricating all of
 * its volume, which is not an error bar, it is a broken instrument. The
 * tolerance is therefore a frame either side by default, and it is a parameter
 * because the right value is a property of the recording's cadence.
 *
 * @par Consumption
 * A print's volume is allocated to at most one fill. Without that, two fills
 * near the same trade would both be called supported, and the audit would
 * report the model as more accurate the more fills it invented.
 *
 * @par Usage
 * Feed the whole tape with @c observe, in time order, then @c audit the run's
 * fills against it. The tape is held rather than streamed because the fills are
 * few and arrive from a finished run, so there is nothing to be gained by
 * making this incremental and a good deal of clarity to be lost.
 */
class tape_audit {
public:
	/**
	 * @brief One frame of a 100ms depth stream, either side.
	 *
	 * The cadence the recording was taken at is the thing that matters, and
	 * this is the default that stream publishes at. A capture taken at 1000ms
	 * wants ten times this, and passing the wrong one shows up as a model that
	 * looks far worse than it is.
	 */
	static constexpr std::uint64_t DEFAULT_TOLERANCE_NS = 100'000'000;

	/// @brief Record one print from the venue's tape. Time order, please -
	///        @c audit binary-searches this and will mis-window a shuffled one.
	STRATEGY_EXPORT void on_trade(market_data::trade_print print);

	/// @brief Prints recorded so far.
	[[nodiscard]] STRATEGY_EXPORT std::size_t size() const noexcept;

	/**
	 * @brief Reconcile @p fills against the tape.
	 *
	 * @param fills The run's modelled passive fills, in time order.
	 * @param spec The listing, for converting the engine's ticks and lots into
	 *        the venue's scaled numbers. The tape speaks the venue's units and
	 *        a fill speaks the engine's; something has to bridge them and only
	 *        the spec can.
	 * @param tolerance_ns Half-window either side of a fill's stamp.
	 * @return What the tape supports, and what it does not.
	 *
	 * @note Non-destructive: the consumption is tracked in scratch state reset
	 *       on entry, so the same tape can audit several runs.
	 */
	[[nodiscard]] STRATEGY_EXPORT tape_audit_report
	audit(std::span<const modelled_fill> fills, const engine::symbol_spec &spec,
		  std::uint64_t tolerance_ns = DEFAULT_TOLERANCE_NS);

private:
	/// @brief A print, reduced to the four fields the match needs.
	struct row {
		std::uint64_t at_ns = 0;
		scaled_price_t price{};
		scaled_qty_t qty{};
		side_t aggressor = side_t::bid;
	};

	/// @brief Index of the first row at or after @p at_ns.
	[[nodiscard]] std::size_t lower_bound(std::uint64_t at_ns) const noexcept;

	std::vector<row> tape_;
	/// @brief Per-row volume already allocated to some fill, parallel to
	///        @c tape_. Cleared by every @c audit so the call is repeatable.
	std::vector<scaled_qty_t> taken_;
};

/*
 * The auditor is a plain tape handler, so the same `market_data::drive` that
 * runs a live tape runs this one. Stated as an assertion rather than left to be
 * discovered at the one call site: it is the property that keeps the audit
 * reading the venue's feed through production code rather than a parser written
 * for the test.
 */
static_assert(market_data::trade_handler<tape_audit>);

} // namespace exchange::strategy::backtest

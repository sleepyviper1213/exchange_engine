#pragma once
// One passive fill the model claims happened, kept so it can be checked.
//
// A backtest's passive fills are an *inference*: the recording carries depth,
// not our executions, so `crossing_fill_model` decides we were filled because
// the venue's book moved through a price we were resting at. That inference is
// the single largest assumption in any result the harness produces, and until
// something compares it against the tape it is unfalsifiable.
//
// This is the claim, written down in the form the comparison needs. @see
// tape_audit.hpp, which is the comparison.

#include "fwd.hpp"
#include "orders/types.hpp"

#include <cstdint>

namespace exchange::strategy::backtest {

/**
 * @brief A passive execution the fill model inferred, as evidence to test.
 *
 * @note Market time, ticks and lots - the engine's own units, not the venue's.
 *       Converting to the venue's scale is the auditor's job, because it needs
 *       the @c symbol_spec to do it and this record must not carry one.
 */
struct modelled_fill {
	/// @brief Market time of the frame the fill was inferred in.
	///
	/// Not when the trade happened. A diff feed publishes on a timer, so the
	/// venue event that filled us occurred somewhere in the frame *before* this
	/// stamp - which is exactly why the audit matches within a window rather
	/// than on equality. @see tape_audit::DEFAULT_TOLERANCE_NS
	std::uint64_t at_ns = 0;

	/// @brief Execution price, in ticks.
	price_t price = NO_PRICE;

	/// @brief The side we were resting on. The aggressor was on the other one,
	///        which is what the tape records.
	side_t our_side = side_t::bid;

	/// @brief Lots we were filled for.
	volume_t volume = {};
};

} // namespace exchange::strategy::backtest

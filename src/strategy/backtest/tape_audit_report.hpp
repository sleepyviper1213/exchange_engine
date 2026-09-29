#pragma once
// How much of what the fill model claimed the tape actually supports.

#include "fwd.hpp"
#include "orders/types.hpp"

#include <cstdint>

namespace exchange::strategy::backtest {

/**
 * @brief The fill model's error bar, in both directions the evidence allows.
 *
 * @par What "supported" means
 * A modelled fill is supported to the extent that the venue's own tape printed,
 * at the same price, on the aggressing side that would have hit us, within the
 * matching window. Print volume is *consumed* as it is allocated, so two fills
 * cannot both be justified by one trade - double-counting would flatter the
 * model, which is the one direction an audit must not err in.
 *
 * @par What it does not mean
 * A supported fill is not a fill that happened. The tape says a trade occurred
 * at our price; it does not say we were the counterparty, and on a venue where
 * we hold no queue position it is nearly certain we were not. This bounds the
 * model from above: volume with no print behind it definitely did not happen,
 * while volume with a print behind it merely *could* have.
 */
struct tape_audit_report {
	/// @brief Modelled passive fills examined.
	std::uint64_t fills = 0;
	/// @brief Lots those fills claimed.
	volume_t lots_claimed = 0;

	/// @brief Fills the tape covered in full.
	std::uint64_t fills_supported = 0;
	/// @brief Fills the tape covered in part.
	std::uint64_t fills_partial = 0;
	/// @brief Fills with no supporting print at all.
	std::uint64_t fills_unsupported = 0;

	/// @brief Claimed lots the tape backs.
	volume_t lots_supported = 0;
	/// @brief Claimed lots with no print behind them. The headline number: this
	///        is volume the backtest credited us that the venue never traded.
	volume_t lots_unsupported = 0;

	/**
	 * @brief Fills the tape does not reach, in time.
	 *
	 * Excluded from the supported/unsupported split rather than counted
	 * against the model, because a fill the recording never covered says
	 * nothing about the model - only about the recording. Counting it as
	 * unsupported would measure how well the two captures were started
	 * together, which is not the question.
	 */
	std::uint64_t fills_uncovered = 0;
	/// @brief Lots those fills claimed.
	volume_t lots_uncovered = 0;

	/// @brief Prints seen, and how many were at a price we were filled at.
	std::uint64_t prints_seen = 0;
	/// @brief Half-window the match allowed, in nanoseconds.
	std::uint64_t tolerance_ns = 0;
};

/**
 * @brief Share of claimed lots the tape backs, in basis points of a whole.
 *
 * Integer, in ten-thousandths, for the reason the rest of the reports here are
 * integral: a ratio that went through a @c double on its way to being printed
 * is one nobody can reconcile against the counters it came from.
 *
 * @return 0 when nothing was claimed - an empty run is not 100% accurate.
 */
[[nodiscard]] constexpr std::int64_t
supported_bps(const tape_audit_report &audit) noexcept {
	const volume_t judged = audit.lots_claimed - audit.lots_uncovered;
	return judged > 0 ? audit.lots_supported * 10000 / judged : 0;
}

/// @brief Lots the audit actually judged - claimed, less those the tape never
///        reached.
[[nodiscard]] constexpr volume_t
lots_judged(const tape_audit_report &audit) noexcept {
	return audit.lots_claimed - audit.lots_uncovered;
}

/// @brief Whether every judged lot had a print behind it.
[[nodiscard]] constexpr bool
is_fully_supported(const tape_audit_report &audit) noexcept {
	return audit.fills > audit.fills_uncovered && audit.lots_unsupported == 0;
}

} // namespace exchange::strategy::backtest

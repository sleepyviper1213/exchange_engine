#pragma once

#include "fwd.hpp"
#include "markout_report.hpp"
#include "report.hpp"
#include "symbol/symbol_spec.hpp"
#include "tape_audit_report.hpp"
#include "orders/units_format.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace exchange::strategy::backtest {

/**
 * @brief A report plus the listing it was measured on - what actually gets
 *        printed.
 *
 * A @c report on its own holds ticks, lots and tick-lots, which are exact and
 * meaningless without the grid they sit on: "+41 800 tick-lots" is not a number
 * anybody can act on. The spec is what turns them back into the decimals the
 * venue quotes, so the pair travels together, exactly as
 * @c market_data::book_ladder pairs a book with its decimals rather than making
 * the book carry them.
 */
struct report_summary {
	const report *run;
	const engine::symbol_spec *spec;
};

/**
 * @brief A markout curve plus the listing it was measured on.
 *
 * Pairs with the curve for the reason @c report_summary pairs with a report:
 * half-ticks and half-tick-lots are exact and unreadable, and the spec is what
 * turns them back into the venue's own decimals. @see markout_report
 */
struct markout_summary {
	const markout_report *curve;
	const engine::symbol_spec *spec;
};

/**
 * @brief An audit plus the listing it was measured on.
 *
 * Travels with the spec for the reason the other two summaries do: lots are
 * exact and unreadable without the grid. @see tape_audit_report
 */
struct tape_audit_summary {
	const tape_audit_report *audit;
	const engine::symbol_spec *spec;
};

namespace detail {

/// @brief Render @p scaled, an integer scaled by 10^@p places, as a decimal.
///
/// Signed, and the sign is applied to the whole rather than to the parts: the
/// fractional digits of a negative value are its magnitude's, so formatting
/// @c -1 at two places is @c "-0.01" and not @c "-0.-1".
[[nodiscard]] inline std::string decimal(std::int64_t scaled, int places) {
	if (places <= 0) return fmt::format("{}", scaled);
	std::uint64_t unit = 1;
	for (int i = 0; i < places; ++i) unit *= 10;

	const bool negative      = scaled < 0;
	const std::uint64_t size = negative
								   ? 0U - static_cast<std::uint64_t>(scaled)
								   : static_cast<std::uint64_t>(scaled);
	return fmt::format("{}{}.{:0{}}",
					   negative ? "-" : "",
					   size / unit,
					   size % unit,
					   places);
}

/// @brief A tick-lot quantity in the listing's quote currency.
///
/// One tick-lot is one tick of price times one lot of size, so the currency
/// value is @c ticks * @c tick_scaled * @c lot_scaled at a scale of
/// @c price_scale + @c qty_scale. Integer throughout - a P&L that went through
/// a @c double on its way to being printed is a P&L nobody can reconcile.
[[nodiscard]] inline std::string money(notional_t tick_lots,
									   const engine::symbol_spec &spec) {
	return decimal(tick_lots.numerical_value_in(units::tick * units::lot) *
					   scaled_of(spec.tick_scaled()) *
					   scaled_of(spec.lot_scaled()),
				   spec.price_scale() + spec.qty_scale());
}

/// @brief A half-tick quantity as ticks, to one decimal.
///
/// Exact: halving is the same as multiplying by five and moving the point one
/// place, so a markout of an odd number of half-ticks prints as @c .5 rather
/// than being rounded into one of its neighbours.
[[nodiscard]] inline std::string ticks_from_half(half_ticks_t half_ticks) {
	return decimal(half_ticks.numerical_value_in(units::half_tick) * 5, 1);
}

/// @brief A half-tick-lot quantity in the listing's quote currency.
///
/// @c money, with the same halving trick: one more decimal place and a factor
/// of five, so nothing is lost on an odd half-tick. @see money
[[nodiscard]] inline std::string
money_from_half(half_tick_lots_t half_tick_lots,
				const engine::symbol_spec &spec) {
	return decimal(half_tick_lots.numerical_value_in(units::half_tick *
													 units::lot) *
					   scaled_of(spec.tick_scaled()) *
					   scaled_of(spec.lot_scaled()) * 5,
				   spec.price_scale() + spec.qty_scale() + 1);
}

/// @brief A horizon as the unit a reader thinks in.
[[nodiscard]] inline std::string horizon(std::uint64_t ns) {
	if (ns >= 1'000'000'000) return fmt::format("{}s", ns / 1'000'000'000);
	if (ns >= 1'000'000) return fmt::format("{}ms", ns / 1'000'000);
	if (ns >= 1000) return fmt::format("{}us", ns / 1000);
	return fmt::format("{}ns", ns);
}

/// @brief Nanoseconds of market time as a human span. Not a latency, so seconds
///        with three decimals is the resolution worth showing.
[[nodiscard]] inline std::string market_time(std::uint64_t ns) {
	if (ns == 0) return "no stamped time";
	return fmt::format("{}s",
					   decimal(static_cast<std::int64_t>(ns / 1'000'000), 3));
}

} // namespace detail
} // namespace exchange::strategy::backtest

/**
 * @brief Prints a run as a block of aligned lines, grouped the way it should be
 *        read: the recording first, then the engine, then the trading.
 *
 * @c nested_formatter so fill, align and width apply to the whole block - the
 * convention the rest of the tree's composite formatters follow.
 */
template <>
struct fmt::formatter<exchange::strategy::backtest::report_summary>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::strategy::backtest::report_summary &summary,
				format_context &ctx) const {
		namespace bt          = exchange::strategy::backtest;
		const bt::report &run = *summary.run;
		const exchange::engine::symbol_spec &spec = *summary.spec;

		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(
				out,
				"backtest {}  [{}]\n"
				"  feed      {} events ({} applied, {} buffered, {} discarded)"
				", {} gaps, {} snapshots\n"
				"  market    {} of market time, replica {}\n"
				"  engine    {} commands ({} depth), {} misroutes, {} stalls, "
				"{} in flight\n"
				"  orders    {} accepted, {} rejected ({} by risk), "
				"{} cancelled, {} cancels declined\n"
				"  amends    {} applied, {} declined\n"
				"  fills     {} total: {} passive ({} lots), "
				"{} aggressive ({} lots), {} self\n"
				"  model     {} aggressors injected, {} lots of venue depth "
				"consumed and restored\n"
				"  queue     {} lots filled ahead of ours\n"
				"  position  {} lots net ({} bought, {} sold), marked at {}\n"
				"  P&L       {} ({} tick-lots){}",
				spec.symbol(),
				is_clean(run) ? "clean" : "SUSPECT - see the counters below",
				run.events_seen,
				run.events_applied,
				run.events_buffered,
				run.events_discarded,
				run.gaps,
				run.snapshots,
				bt::detail::market_time(covered_ns(run)),
				run.live_at_end ? "live" : "NOT LIVE",
				run.commands_applied,
				run.depth_commands,
				run.misroutes,
				run.queue_stalls,
				run.commands_in_flight,
				run.orders_accepted,
				run.orders_rejected,
				run.risk_refusals,
				run.orders_cancelled,
				run.cancels_rejected,
				run.orders_amended,
				run.amends_rejected,
				total_fills(run),
				run.passive_fills,
				run.passive_lots,
				run.aggressive_fills,
				run.aggressive_lots,
				run.self_fills,
				run.injected_aggressors,
				run.depth_consumed_lots,
				run.queue_absorbed_lots,
				run.net_lots,
				run.bought_lots,
				run.sold_lots,
				bt::detail::decimal(
					exchange::scaled_of(spec.price_to_scaled(run.mark)),
					spec.price_scale()),
				bt::detail::money(run.pnl_tick_lots, spec),
				run.pnl_tick_lots,
				run.breaker_tripped ? "  [BREAKER TRIPPED]" : "");
		});
	}
};

/**
 * @brief Prints a markout curve as one row per horizon.
 *
 * Passive first and widest, because that is the column the question is about:
 * a passive markout that slopes down as the horizon lengthens is the strategy
 * being picked off. The aggressive column is there to be compared against it -
 * crossing the spread costs what it costs, and it should not slope.
 *
 * A row whose sample is mostly unresolved is marked rather than dropped. The
 * average is still printed, because suppressing it would hide how many fills
 * were involved, but the marker says not to read it: the fills that resolved
 * are exactly the early ones. @see is_covered
 */
template <>
struct fmt::formatter<exchange::strategy::backtest::markout_summary>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::strategy::backtest::markout_summary &summary,
				format_context &ctx) const {
		namespace bt                  = exchange::strategy::backtest;
		const bt::markout_report &out = *summary.curve;
		const exchange::engine::symbol_spec &spec = *summary.spec;

		return write_padded(ctx, [&](auto out_it) {
			out_it = fmt::format_to(
				out_it,
				"markout {}  (ticks per lot; positive is in our favour)\n"
				"  horizon   passive                    aggressive\n",
				spec.symbol());

			if (out.count == 0)
				return fmt::format_to(out_it, "  (not recorded)");

			for (std::size_t i = 0; i < out.count; ++i) {
				const auto &at = out.horizons[i];
				// The marker is about a *biased sample*, so it needs a sample
				// to be about. `is_covered` is false for an empty bucket too -
				// correctly, nothing is covered - but printing "most fills
				// outlived the run" against a row of zeroes states something
				// that did not happen. A run with no fills says nothing here.
				const bool any = resolved_fills(at) + at.unresolved > 0;
				out_it         = fmt::format_to(
					out_it,
					"  {:>7}   {:>6} @ {:>6} lots  {:>9}   "
					"{:>6} @ {:>6} lots  {:>9}{}\n",
					bt::detail::horizon(at.horizon_ns),
					at.passive_fills,
					at.passive_lots,
					mp_units::is_gt_zero(at.passive_lots)
						? bt::detail::ticks_from_half(
							  bt::passive_markout_per_lot(at))
						: std::string{"-"},
					at.aggressive_fills,
					at.aggressive_lots,
					mp_units::is_gt_zero(at.aggressive_lots)
						? bt::detail::ticks_from_half(
							  bt::aggressive_markout_per_lot(at))
						: std::string{"-"},
					any && !is_covered(at)
						? "  [thin: most fills outlived the run]"
						: "");
			}

			const auto &last = out.horizons[out.count - 1];
			return fmt::format_to(
				out_it,
				"  passive total at {}: {} ({} half-tick-lots)",
				bt::detail::horizon(last.horizon_ns),
				bt::detail::money_from_half(last.passive_half_tick_lots, spec),
				last.passive_half_tick_lots);
		});
	}
};

/**
 * @brief Prints how much of the fill model's claim the tape stands behind.
 *
 * The unsupported line is the one that matters and it is deliberately phrased
 * as a bound rather than a verdict: volume with no print behind it did not
 * happen, while volume with a print behind it merely could have, since the tape
 * does not say we were the counterparty. @see tape_audit_report
 */
template <>
struct fmt::formatter<exchange::strategy::backtest::tape_audit_summary>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::strategy::backtest::tape_audit_summary &summary,
				format_context &ctx) const {
		namespace bt                    = exchange::strategy::backtest;
		const bt::tape_audit_report &at = *summary.audit;

		return write_padded(ctx, [&](auto out) {
			if (at.fills == 0)
				return fmt::format_to(
					out,
					"tape audit  no modelled passive fills to check "
					"({} prints on the tape)",
					at.prints_seen);

			return fmt::format_to(
				out,
				"tape audit  [{}]\n"
				"  window    {}ms either side of the frame a fill was "
				"inferred in\n"
				"  tape      {} prints\n"
				"  claimed   {} fills, {} lots ({} outside the tape's own "
				"window, set aside)\n"
				"  backed    {} fills in full, {} in part, {} not at all\n"
				"  lots      {} backed, {} with no print behind them\n"
				"  support   {}.{:02}% of the {} lots judged, as an upper "
				"bound",
				bt::is_fully_supported(at)
					? "every claimed lot has a print behind it"
					: "the model claimed volume the tape does not show",
				at.tolerance_ns / 1'000'000,
				at.prints_seen,
				at.fills,
				at.lots_claimed,
				at.fills_uncovered,
				at.fills_supported,
				at.fills_partial,
				at.fills_unsupported,
				at.lots_supported,
				at.lots_unsupported,
				bt::supported_bps(at) / 100,
				bt::supported_bps(at) % 100,
				bt::lots_judged(at));
		});
	}
};

#pragma once
// Opt-in fmt support for a backtest report - the sidecar shape the tree uses
// everywhere (market-data/format.hpp, trading-engine/format.hpp). Nothing
// includes this implicitly; a translation unit that prints a report asks for
// it.

#include "fwd.hpp"
#include "report.hpp"
#include "symbol/symbol_spec.hpp"

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
[[nodiscard]] inline std::string money(std::int64_t tick_lots,
									   const engine::symbol_spec &spec) {
	return decimal(tick_lots * spec.tick_scaled() * spec.lot_scaled(),
				   spec.price_scale() + spec.qty_scale());
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
				bt::detail::decimal(spec.price_to_scaled(run.mark),
									spec.price_scale()),
				bt::detail::money(run.pnl_tick_lots, spec),
				run.pnl_tick_lots,
				run.breaker_tripped ? "  [BREAKER TRIPPED]" : "");
		});
	}
};

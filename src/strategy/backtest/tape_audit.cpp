#include "tape_audit.hpp"

#include "market_data/trade_print.hpp"
#include "modelled_fill.hpp"
#include "orders/types.hpp"
#include "symbol/symbol_spec.hpp"
#include "tape_audit_report.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

namespace exchange::strategy::backtest {

void tape_audit::on_trade(market_data::trade_print print) {
	tape_.emplace_back(static_cast<std::uint64_t>(print.event_time.count()),
					   print.price,
					   print.qty,
					   print.aggressor);
}

std::size_t tape_audit::size() const noexcept { return tape_.size(); }

std::size_t tape_audit::lower_bound(std::uint64_t at_ns) const noexcept {
	const auto it =
		std::ranges::lower_bound(tape_, at_ns, {}, [](const row &r) {
			return r.at_ns;
		});
	return static_cast<std::size_t>(it - tape_.begin());
}

tape_audit_report tape_audit::audit(std::span<const modelled_fill> fills,
									const engine::symbol_spec &spec,
									std::uint64_t tolerance_ns) {
	taken_.assign(tape_.size(), scaled_qty_t::zero());

	tape_audit_report out;
	out.prints_seen  = static_cast<std::uint64_t>(tape_.size());
	out.tolerance_ns = tolerance_ns;

	// The window the recording can speak for at all. A fill whose own match
	// window lies entirely outside it is not evidence about the model, it is
	// evidence about how the two captures were started. @see fills_uncovered
	const bool have_tape           = !tape_.empty();
	const std::uint64_t tape_from  = have_tape ? tape_.front().at_ns : 0;
	const std::uint64_t tape_until = have_tape ? tape_.back().at_ns : 0;

	for (const modelled_fill &claim : fills) {
		++out.fills;
		out.lots_claimed += claim.volume;

		const auto want = spec.quantity_to_scaled(order_quantity(claim.volume));
		const auto at_price = spec.price_to_scaled(claim.price);
		// The tape names the *aggressor*. We were resting, so whoever hit us
		// was on the other side: a fill of our bid was somebody selling.
		const side_t hitter = opposed(claim.our_side);

		const std::uint64_t from =
			claim.at_ns > tolerance_ns ? claim.at_ns - tolerance_ns : 0;
		const std::uint64_t until = claim.at_ns > UINT64_MAX - tolerance_ns
										? UINT64_MAX
										: claim.at_ns + tolerance_ns;

		if (!have_tape || until < tape_from || from > tape_until) {
			++out.fills_uncovered;
			out.lots_uncovered += claim.volume;
			continue;
		}

		scaled_qty_t found = scaled_qty_t::zero();
		for (std::size_t i = lower_bound(from);
			 i < tape_.size() && tape_[i].at_ns <= until && found < want;
			 ++i) {
			const row &print = tape_[i];
			if (print.price != at_price || print.aggressor != hitter) continue;

			const auto spare = print.qty - taken_[i];
			if (spare <= scaled_qty_t::zero()) continue;

			const auto use = std::min(spare, want - found);
			taken_[i] += use;
			found += use;
		}

		// Back into lots, rounding *down*: a partial lot of evidence does not
		// support a whole lot of claim, and rounding the other way would let a
		// sliver of print volume justify a fill it cannot pay for.
		const volume_t backed    = spec.volume_from_scaled(found);
		const volume_t supported = std::min(backed, claim.volume);

		out.lots_supported += supported;
		out.lots_unsupported += claim.volume - supported;
		if (supported == claim.volume) ++out.fills_supported;
		else if (mp_units::is_gt_zero(supported)) ++out.fills_partial;
		else ++out.fills_unsupported;
	}

	return out;
}

} // namespace exchange::strategy::backtest

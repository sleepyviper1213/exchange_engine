#include "venue_fill_book.hpp"

#include "venue_bridge.hpp"

#include <algorithm>

namespace exchange::session {

venue_fill_book::venue_fill_book() noexcept { recent_trades_.fill(-1); }

void venue_fill_book::remember(std::int64_t venue_id, order_id_t id) noexcept {
	for (const auto &[known, ours] : venue_orders_)
		if (known == venue_id) return;
	venue_orders_[next_venue_order_] = {venue_id, id};
	next_venue_order_ = (next_venue_order_ + 1) % venue_orders_.size();
}

order_id_t venue_fill_book::engine_id_of(std::int64_t venue_id) const noexcept {
	for (const auto &[known, ours] : venue_orders_)
		if (known == venue_id) return ours;
	return 0;
}

bool venue_fill_book::is_repeated_trade(std::int64_t trade_id) noexcept {
	if (trade_id < 0) return false;
	if (std::ranges::contains(recent_trades_, trade_id)) return true;
	newest_trade_id_                 = std::max(newest_trade_id_, trade_id);
	recent_trades_[next_trade_slot_] = trade_id;
	next_trade_slot_ = (next_trade_slot_ + 1) % recent_trades_.size();
	return false;
}

std::int64_t venue_fill_book::newest_trade_id() const noexcept {
	return newest_trade_id_;
}

std::expected<valued_fill, unbooked_fill>
venue_fill_book::value_fill(const venue::execution_report &report,
							const engine::symbol_spec &spec,
							std::optional<side_t> side_hint) {
	const auto lots = lots_from(report.last_qty_scaled, spec);
	if (!lots || mp_units::is_lteq_zero(*lots))
		return std::unexpected(unbooked_fill::NO_QUANTITY);
	// Off the tick grid means the report is about a listing whose reference
	// data we have wrong, and marking a position at a made-up price is worse
	// than not marking it.
	const auto price =
		spec.price_from_scaled(at_scaled(report.last_price_scaled));
	if (!price) return std::unexpected(unbooked_fill::OFF_GRID);

	// The venue's word first; booking on a guessed side is how the position
	// drifted, and not booking it is at least visible.
	const std::optional<side_t> ours =
		report.side.has_value() ? report.side : side_hint;
	if (!ours) return std::unexpected(unbooked_fill::SIDE_UNKNOWN);

	const timestamp_t at =
		report.transaction_time_ms <= 0
			? 0
			: static_cast<timestamp_t>(report.transaction_time_ms) * 1'000'000U;
	return valued_fill{.side           = *ours,
					   .price          = *price,
					   .lots           = *lots,
					   .at             = at,
					   .aggressor_side = report.is_maker ? opposed(*ours) : *ours};
}

std::optional<valued_fee>
venue_fill_book::value_fee(const venue::execution_report &report,
						   const engine::symbol_spec &spec,
						   std::string_view quote_asset,
						   std::string_view base_asset) {
	if (report.commission_scaled <= 0) return valued_fee{.charge = {}};
	// What one tick-lot is worth, at the commission's scale. A listing with no
	// USDT grid has no money value, and its fees are unvalued.
	const auto per_tick_lot = spec.usdt_from(1 * (units::tick * units::lot));
	if (!per_tick_lot || report.commission_asset.empty()) return std::nullopt;
	static_assert(venue::COMMISSION_SCALE == USDT_SCALE,
				  "a quote-asset fee is read straight as usdt_e8");
	const std::int64_t unit = usdt_e8_of(*per_tick_lot);

	std::int64_t value_e8    = 0;
	std::int64_t base_scaled = 0;
	if (report.commission_asset == quote_asset) {
		value_e8 = report.commission_scaled;
	} else if (report.commission_asset == base_asset) {
		// The fee's size at the price it traded at: scaled base times a price
		// at price_scale, brought back to 1e-8 of the quote.
		value_e8 = report.commission_scaled * report.last_price_scaled;
		for (int i = 0; i < spec.price_scale(); ++i) value_e8 /= 10;
		base_scaled = report.commission_scaled;
	} else {
		return std::nullopt;
	}
	const std::int64_t tick_lots = (value_e8 + unit - 1) / unit;
	return valued_fee{.charge      = tick_lots * (units::tick * units::lot),
					  .base_scaled = base_scaled};
}

} // namespace exchange::session

#include "venue_bridge.hpp"

#include "event/command.hpp"
#include "order_book/reject_reason.hpp"
#include "session/client_order_id.hpp"

#include <algorithm>

namespace exchange::session {
std::expected<venue::outbound_order, bridge_error>
to_outbound_order(const engine::orders::order &order,
				  const engine::symbol_spec &spec,
				  std::string_view venue_symbol) {
	return venue::outbound_order{
		.symbol          = std::string(venue_symbol),
		.client_order_id = client_order_id(order.id),
		.side            = order.side,
		.type            = order.type,
		.tif             = order.tif,
		.price_scaled    = spec.price_to_scaled(order.price),
		.price_scale     = spec.price_scale(),
		.qty_scaled      = spec.quantity_to_scaled(order.qty),
		.qty_scale       = spec.qty_scale()};
}

venue::outbound_cancel to_outbound_cancel(order_id_t id,
										  std::string_view venue_symbol) {
	return venue::outbound_cancel{.symbol          = std::string(venue_symbol),
								  .client_order_id = client_order_id(id)};
}

std::expected<quantity_t, bridge_error>
lots_from(std::int64_t scaled, const engine::symbol_spec &spec) {
	if (scaled == 0) return quantity_t{0};
	const auto lots = spec.quantity_from_scaled(scaled);
	if (!lots) return std::unexpected(bridge_error::quantity_off_grid);
	return *lots;
}

std::expected<engine::order_outcome, bridge_error>
to_outcome(const venue::execution_report &report,
		   const engine::symbol_spec &spec) {
	const auto id = engine_order_id(report.subject_order_id());
	if (!id) return std::unexpected(bridge_error::unknown_order);

	const auto traded = lots_from(report.cumulative_qty_scaled, spec);
	if (!traded) return std::unexpected(traded.error());
	const auto ordered = lots_from(report.order_qty_scaled, spec);
	if (!ordered) return std::unexpected(ordered.error());
	if (*traded > *ordered) return std::unexpected(bridge_error::out_of_range);

	return engine::order_outcome{
		.id     = *id,
		.type   = to_outcome_type(report.kind),
		.reason = report.status == venue::execution_status::rejected
					  ? engine::reject_reason::VENUE_REJECTED
					  : engine::reject_reason::NONE,
		.status = to_engine_status(report.status, *traded > 0),
		.traded = *traded,
		// What the venue still has working. Derived rather than taken from the
		// venue, which does not send a remaining quantity - and a status can be
		// terminal with quantity unexecuted, which is what a cancel is.
		.remaining = static_cast<quantity_t>(*ordered - *traded)};
}

std::vector<reconciled_order>
reconcile(std::span<const order_id_t> ours,
		  std::span<const std::string> venue_open) {
	std::vector<reconciled_order> found;
	found.reserve(ours.size() + venue_open.size());

	for (const std::string &open : venue_open) {
		const auto id = engine_order_id(open);
		if (!id) {
			found.emplace_back(open, 0, reconciliation::foreign);
			continue;
		}
		const bool known = std::ranges::contains(ours, *id);
		found.emplace_back(open,
						   *id,
						   known ? reconciliation::agreed
								 : reconciliation::adopt);
	}

	for (const order_id_t id : ours) {
		const std::string& mine = client_order_id(id);
		if (std::ranges::contains(venue_open, mine)) continue;
		found.emplace_back(mine, id, reconciliation::presumed_gone);
	}
	return found;
}
} // namespace exchange::session
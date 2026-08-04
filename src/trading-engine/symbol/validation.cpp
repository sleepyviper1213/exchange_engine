#include "validation.hpp"

namespace exchange::engine {

std::expected<order, reject_reason> validate(const order_request &request,
											const symbol_spec &spec) noexcept {
	const auto price = spec.price_from_text(request.price);
	if (!price) return std::unexpected(price.error());

	// The collar is checked after the tick grid on purpose: "not on the tick"
	// is the more specific complaint, and a price far outside the band is
	// usually also misaligned. Reporting the grid error first tells the client
	// the thing it can actually fix.
	if (!spec.within_collar(*price))
		return std::unexpected(reject_reason::PRICE_OUTSIDE_COLLAR);

	const auto qty = spec.quantity_from_text(request.quantity);
	if (!qty) return std::unexpected(qty.error());

	// A trigger price and a stop order imply each other, both ways. Accepting a
	// STOP without one would leave a trigger that never fires; accepting one on
	// a LIMIT would silently ignore a price the client clearly meant something
	// by. Neither is a thing to guess at.
	const bool is_stop = request.type == order_type::STOP;
	if (is_stop && request.stop_price.empty())
		return std::unexpected(reject_reason::MISSING_STOP_PRICE);
	if (!is_stop && !request.stop_price.empty())
		return std::unexpected(reject_reason::UNEXPECTED_STOP_PRICE);

	price_t stop_ticks = 0;
	if (is_stop) {
		// Same grid and same band as the limit price: a trigger the venue could
		// never print is a trigger that never fires.
		const auto trigger = spec.price_from_text(request.stop_price);
		if (!trigger) return std::unexpected(trigger.error());
		if (!spec.within_collar(*trigger))
			return std::unexpected(reject_reason::PRICE_OUTSIDE_COLLAR);
		stop_ticks = *trigger;
	}

	return order{.id         = request.id,
				 .side       = request.side,
				 .type       = request.type,
				 .tif        = request.tif,
				 .price      = *price,
				 .stop_price = stop_ticks,
				 .qty        = *qty,
				 .timestamp  = request.timestamp};
}

void symbol_registry::add(symbol_spec spec) {
	// insert_or_assign, not emplace: re-registering a listing is how an
	// operator corrects reference data, and silently keeping the stale one
	// would be the worst of the three possible behaviours.
	by_id.insert_or_assign(spec.id(), std::move(spec));
}

const symbol_spec *symbol_registry::find(symbol_id_t id) const noexcept {
	const auto it = by_id.find(id);
	return it == by_id.end() ? nullptr : &it->second;
}

std::expected<order, reject_reason>
symbol_registry::validate(const order_request &request) const noexcept {
	const symbol_spec *spec = find(request.symbol);
	if (spec == nullptr) return std::unexpected(reject_reason::UNKNOWN_SYMBOL);
	return exchange::engine::validate(request, *spec);
}

} // namespace exchange::engine

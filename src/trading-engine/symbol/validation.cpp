#include "validation.hpp"

namespace exchange::engine {

std::expected<Order, reject_reason> validate(const OrderRequest &request,
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

	return Order{.id        = request.id,
				 .side      = request.side,
				 .price     = *price,
				 .qty       = *qty,
				 .type      = request.type,
				 .timestamp = request.timestamp};
}

void SymbolRegistry::add(symbol_spec spec) {
	// insert_or_assign, not emplace: re-registering a listing is how an
	// operator corrects reference data, and silently keeping the stale one
	// would be the worst of the three possible behaviours.
	by_id.insert_or_assign(spec.id(), std::move(spec));
}

const symbol_spec *SymbolRegistry::find(symbol_id_t id) const noexcept {
	const auto it = by_id.find(id);
	return it == by_id.end() ? nullptr : &it->second;
}

std::expected<Order, reject_reason>
SymbolRegistry::validate(const OrderRequest &request) const noexcept {
	const symbol_spec *spec = find(request.symbol);
	if (spec == nullptr) return std::unexpected(reject_reason::UNKNOWN_SYMBOL);
	return exchange::engine::validate(request, *spec);
}

} // namespace exchange::engine

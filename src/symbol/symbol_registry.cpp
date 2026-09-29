#include "symbol_registry.hpp"

#include <utility>

namespace exchange::engine {

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

std::expected<orders::order, reject_reason>
symbol_registry::validate(const order_request &request) const noexcept {
	const symbol_spec *spec = find(request.symbol);
	if (spec == nullptr) return std::unexpected(reject_reason::UNKNOWN_SYMBOL);
	return exchange::engine::validate(request, *spec);
}

} // namespace exchange::engine

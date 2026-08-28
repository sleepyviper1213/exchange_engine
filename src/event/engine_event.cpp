#include "engine_event.hpp"

#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"
#include "orders/types.hpp"

namespace exchange::engine::event {

engine_event::engine_event() noexcept
	: symbol(0), kind(EventKind::TRADE), execution_{} {}

[[nodiscard]] const engine::trade &engine_event::as_trade() const noexcept {
	assert(kind == EventKind::TRADE);
	return execution_; // NOLINT(cppcoreguidelines-pro-type-union-access)
}

[[nodiscard]] const engine::order_outcome &
engine_event::as_outcome() const noexcept {
	assert(kind == EventKind::OUTCOME);
	return lifecycle_; // NOLINT(cppcoreguidelines-pro-type-union-access)
}


engine_event engine_event::of(symbol_id_t symbol,
							  const engine::trade &execution) noexcept {
	return {symbol, execution};
}

engine_event engine_event::of(symbol_id_t symbol,
							  const engine::order_outcome &record) noexcept {
	return {symbol, record};
}

engine_event::engine_event(symbol_id_t listing,
						   const engine::trade &execution) noexcept
	: symbol(listing), kind(EventKind::TRADE), execution_(execution) {}

engine_event::engine_event(symbol_id_t listing,
						   const engine::order_outcome &record) noexcept
	: symbol(listing), kind(EventKind::OUTCOME), lifecycle_(record) {}

// Comparing a tagged union means comparing the arm the tag names; a memberwise
// default would read the wider arm through the narrower one and compare
// padding.
bool engine_event::operator==(const engine_event &other) const noexcept {
	if (symbol != other.symbol || kind != other.kind) return false;
	switch (kind) {
	case EventKind::TRADE: return as_trade() == other.as_trade();
	case EventKind::OUTCOME: return as_outcome() == other.as_outcome();
	}
	return false;
}

} // namespace exchange::engine::event

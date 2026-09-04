#include "engine_event.hpp"

#include "order_book/outcome.hpp"
#include "order_book/trade.hpp"
#include "orders/types.hpp"

namespace exchange::engine::event {

engine_event::engine_event() noexcept
	: symbol_(0), kind_(event_kind::TRADE), execution_{} {}

[[nodiscard]] const engine::trade &engine_event::as_trade() const noexcept {
	assert(kind_ == event_kind::TRADE);
	return execution_; // NOLINT(cppcoreguidelines-pro-type-union-access)
}

[[nodiscard]] const engine::order_outcome &
engine_event::as_outcome() const noexcept {
	assert(kind_ == event_kind::OUTCOME);
	return lifecycle_; // NOLINT(cppcoreguidelines-pro-type-union-access)
}


engine_event engine_event::of(symbol_id_t symbol_,
							  const engine::trade &execution) noexcept {
	return {symbol_, execution};
}

engine_event engine_event::of(symbol_id_t symbol_,
							  const engine::order_outcome &record) noexcept {
	return {symbol_, record};
}

engine_event::engine_event(symbol_id_t listing,
						   const engine::trade &execution) noexcept
	: symbol_(listing), kind_(event_kind::TRADE), execution_(execution) {}

engine_event::engine_event(symbol_id_t listing,
						   const engine::order_outcome &record) noexcept
	: symbol_(listing), kind_(event_kind::OUTCOME), lifecycle_(record) {}

// Comparing a tagged union means comparing the arm the tag names; a memberwise
// default would read the wider arm through the narrower one and compare
// padding.
bool engine_event::operator==(const engine_event &other) const noexcept {
	if (symbol_ != other.symbol_ || kind_ != other.kind_) return false;
	switch (kind_) {
	case event_kind::TRADE: return as_trade() == other.as_trade();
	case event_kind::OUTCOME: return as_outcome() == other.as_outcome();
	}
	return false;
}

symbol_id_t engine_event::symbol() const { return symbol_; }

event_kind engine_event::kind() const { return kind_; }


} // namespace exchange::engine::event

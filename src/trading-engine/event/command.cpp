#include "command.hpp"

namespace exchange::engine::event {
command command::place(const order &o) noexcept { return command(o); }

command command::cancel(symbol_id_t symbol, order_id_t id) noexcept {
	return {Type::CANCEL, symbol, id};
}

command command::add(symbol_id_t symbol, side_t side, price_t price,
					 quantity_t volume) noexcept {
	return command(Type::ADD, symbol, level_change{side, price, volume});
}

command command::reduce(symbol_id_t symbol, side_t side, price_t price,
						quantity_t volume) noexcept {
	return command(Type::REDUCE, symbol, level_change{side, price, volume});
}

// A validated order already records its listing, so PLACE takes the routing key
// off the payload rather than asking the caller to repeat it — the two can then
// never disagree.
command::command(const order &o) noexcept
	: type(Type::PLACE), symbol(o.symbol_id), order_(o) {}

command::command(Type t, symbol_id_t symbol, order_id_t id) noexcept
	: type(t), symbol(symbol), cancel_id(id) {}

command::command(Type t, symbol_id_t symbol, level_change lc) noexcept
	: type(t), symbol(symbol), level(lc) {}
} // namespace event

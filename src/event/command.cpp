#include "command.hpp"

namespace exchange::engine::event {
[[nodiscard]] const order &command::as_place() const noexcept {
	assert(type == command_type::PLACE);
	return order_; // NOLINT(cppcoreguidelines-pro-type-union-access)
}

[[nodiscard]] order_id_t command::as_cancel() const noexcept {
	assert(type == command_type::CANCEL);
	return cancel_id; // NOLINT(cppcoreguidelines-pro-type-union-access)
}

[[nodiscard]] const level_change &command::as_level() const noexcept {
	assert(type == command_type::ADD || type == command_type::REDUCE);
	return level; // NOLINT(cppcoreguidelines-pro-type-union-access)
}


command command::place(const order &o) noexcept { return command(o); }

command command::cancel(symbol_id_t symbol, order_id_t id) noexcept {
	return {command_type::CANCEL, symbol, id};
}

command command::add(symbol_id_t symbol, side_t side, price_t price,
					 quantity_t volume) noexcept {
	return command(command_type::ADD, symbol, level_change{side, price, volume});
}

command command::reduce(symbol_id_t symbol, side_t side, price_t price,
						quantity_t volume) noexcept {
	return command(command_type::REDUCE, symbol, level_change{side, price, volume});
}

// A validated order already records its listing, so PLACE takes the routing key
// off the payload rather than asking the caller to repeat it - the two can then
// never disagree.
command::command(const order &o) noexcept
	: type(command_type::PLACE), symbol(o.symbol_id), order_(o) {}

command::command(command_type t, symbol_id_t listing, order_id_t id) noexcept
	: type(t), symbol(listing), cancel_id(id) {}

command::command(command_type t, symbol_id_t listing, level_change lc) noexcept
	: type(t), symbol(listing), level(lc) {}
} // namespace event

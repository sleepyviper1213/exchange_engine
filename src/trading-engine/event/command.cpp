#include "command.hpp"

namespace exchange::engine::event {
command command::place(const order &o) noexcept { return command(o); }

command command::cancel(order_id_t id) noexcept {
	return command(Type::CANCEL, id);
}

command command::add(side_t side, price_t price, quantity_t volume) noexcept {
	return command(Type::ADD, level_change{side, price, volume});
}

command command::reduce(side_t side, price_t price, quantity_t volume) noexcept {
	return command(Type::REDUCE, level_change{side, price, volume});
}

command::command(const order &o) noexcept : type(Type::PLACE), order_(o) {}

command::command(Type t, order_id_t id) noexcept : type(t), cancel_id(id) {}

command::command(Type t, level_change lc) noexcept : type(t), level(lc) {}
} // namespace event
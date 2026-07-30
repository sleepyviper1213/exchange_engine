#include "command.hpp"

namespace exchange::engine::event {
Command Command::place(const Order &o) noexcept { return Command(o); }

Command Command::cancel(order_id_t id) noexcept {
	return Command(Type::CANCEL, id);
}

Command Command::add(side_t side, price_t price, quantity_t volume) noexcept {
	return Command(Type::ADD, LevelChange{side, price, volume});
}

Command Command::reduce(side_t side, price_t price, quantity_t volume) noexcept {
	return Command(Type::REDUCE, LevelChange{side, price, volume});
}

Command Command::set_level(side_t side, price_t price, quantity_t volume) noexcept {
	return Command(Type::SET_LEVEL, LevelChange{side, price, volume});
}

Command::Command(const Order &o) noexcept : type(Type::PLACE), order(o) {}

Command::Command(Type t, order_id_t id) noexcept : type(t), cancel_id(id) {}

Command::Command(Type t, LevelChange lc) noexcept : type(t), level(lc) {}
} // namespace event
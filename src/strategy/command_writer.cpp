#include "command_writer.hpp"

#include "orders/amendment.hpp"

namespace exchange::strategy {
command_writer::command_writer(engine::event::command *first,
							   std::size_t capacity,
							   symbol_id_t symbol) noexcept
	: begin_(first), cur_(first), end_(first + capacity), symbol_(symbol) {}

void command_writer::place(engine::orders::order o) noexcept {
	o.symbol_id = symbol_;
	write(engine::event::command::place(o));
}

void command_writer::cancel(order_id_t id) noexcept {
	write(engine::event::command::cancel(symbol_, id));
}

void command_writer::modify(order_id_t id, price_t price, quantity_t quantity,
							timestamp_t at) noexcept {
	write(engine::event::command::modify(
		symbol_,
		engine::orders::amendment{.id        = id,
								  .price     = price,
								  .quantity  = quantity,
								  .timestamp = at}));
}

void command_writer::add(side_t side, price_t price,
						 quantity_t volume) noexcept {
	write(engine::event::command::add(symbol_, side, price, volume));
}

void command_writer::reduce(side_t side, price_t price,
							quantity_t volume) noexcept {
	write(engine::event::command::reduce(symbol_, side, price, volume));
}

[[nodiscard]] std::span<const engine::event::command>
command_writer::written() const noexcept EXCHANGE_LIFETIMEBOUND {
	return {begin_, cur_};
}

void command_writer::reset() noexcept { cur_ = begin_; }

[[nodiscard]] std::size_t command_writer::size() const noexcept {
	return static_cast<std::size_t>(cur_ - begin_);
}

[[nodiscard]] std::size_t command_writer::remaining() const noexcept {
	return static_cast<std::size_t>(end_ - cur_);
}

[[nodiscard]] bool command_writer::empty() const noexcept {
	return cur_ == begin_;
}

[[nodiscard]] symbol_id_t command_writer::symbol() const noexcept {
	return symbol_;
}

void command_writer::write(const engine::event::command &cmd) noexcept {
	assert(cur_ != end_ &&
		   "command_writer overflow: a strategy emitted more commands for "
		   "one event than its MAX_COMMANDS_PER_EVENT promised, or the "
		   "caller dispatched without reserving room first");
	*cur_ = cmd;
	++cur_;
}


} // namespace exchange::strategy
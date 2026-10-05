#include "position.hpp"

#include "detail/position_entry.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>

namespace exchange::risk::hooks::pre_trade {
using detail::bump;
using detail::position_entry;
using detail::working;

position_book::position_book(std::size_t capacity) : entries_(capacity) {}

[[nodiscard]] std::size_t position_book::capacity() const noexcept {
	return entries_.size();
}

[[nodiscard]] bool position_book::carries(symbol_id_t symbol) const noexcept {
	return symbol < entries_.size();
}

void position_book::apply_fill(symbol_id_t symbol, side_t side, price_t price,
							   quantity_t lots) noexcept {
	assert(carries(symbol));
	assert(mp_units::is_gt_zero(lots));
	position_entry &e = entries_[symbol];

	const volume_t qty        = lots;
	const notional_t notional = notional_of(price, qty);
	const bool buying         = side == side_t::bid;

	// signed = buying ? qty : -qty, without a branch. @see units::negated_if
	// Both before either store, so neither outlives a call in a register.
	const volume_t signed_qty = units::negated_if(qty, !buying);
	const notional_t signed_n = units::negated_if(notional, !buying);

	bump(e.net_lots, signed_qty);
	bump(e.net_notional, signed_n);
	bump(buying ? e.bought_lots : e.sold_lots, qty);
}

void position_book::add_working(symbol_id_t symbol, side_t side,
								volume_t lots) noexcept {
	assert(carries(symbol));
	bump(working(entries_[symbol], side), lots);
}

void position_book::remove_working(symbol_id_t symbol, side_t side,
								   volume_t lots) noexcept {
	assert(carries(symbol));
	std::atomic<volume_t> &slot = working(entries_[symbol], side);
	bump(slot, -lots);
	// Working quantity going negative means a retirement was applied twice,
	// or one the gate never counted. Both are ledger bugs and both make
	// every later exposure check too permissive, which is the failure a
	// risk system must not have quietly.
	assert(mp_units::is_gteq_zero(slot.load(std::memory_order_relaxed)) &&
		   "working quantity went negative: an order was retired twice");
}

void position_book::reset(symbol_id_t symbol) noexcept {
	assert(carries(symbol));
	position_entry &e = entries_[symbol];
	e.net_lots.store({}, std::memory_order_relaxed);
	e.net_notional.store({}, std::memory_order_relaxed);
	e.bought_lots.store({}, std::memory_order_relaxed);
	e.sold_lots.store({}, std::memory_order_relaxed);
	e.working_bid_lots.store({}, std::memory_order_relaxed);
	e.working_ask_lots.store({}, std::memory_order_relaxed);
}

[[nodiscard]] volume_t
position_book::net_lots(symbol_id_t symbol) const noexcept {
	assert(carries(symbol));
	return entries_[symbol].net_lots.load(std::memory_order_relaxed);
}

[[nodiscard]] volume_t position_book::working_lots(symbol_id_t symbol,
												   side_t side) const noexcept {
	assert(carries(symbol));
	return working(entries_[symbol], side).load(std::memory_order_relaxed);
}

[[nodiscard]] position_snapshot
position_book::snapshot(symbol_id_t symbol) const noexcept {
	assert(carries(symbol));
	const position_entry &e = entries_[symbol];
	return {
		.net_lots         = e.net_lots.load(std::memory_order_relaxed),
		.net_notional     = e.net_notional.load(std::memory_order_relaxed),
		.bought_lots      = e.bought_lots.load(std::memory_order_relaxed),
		.sold_lots        = e.sold_lots.load(std::memory_order_relaxed),
		.working_bid_lots = e.working_bid_lots.load(std::memory_order_relaxed),
		.working_ask_lots = e.working_ask_lots.load(std::memory_order_relaxed),
	};
}

[[nodiscard]] volume_t position_snapshot::gross_lots() const noexcept {
	const volume_t if_bids_fill = units::abs_of(net_lots + working_bid_lots);
	const volume_t if_asks_fill = units::abs_of(net_lots - working_ask_lots);
	return if_bids_fill > if_asks_fill ? if_bids_fill : if_asks_fill;
}

[[nodiscard]] notional_t position_snapshot::pnl(price_t mark) const noexcept {
	return notional_of(mark, net_lots) - net_notional;
}

} // namespace exchange::risk::hooks::pre_trade

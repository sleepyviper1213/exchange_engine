#include "position.hpp"

namespace exchange::risk {
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
	assert(lots > 0);
	entry &e = entries_[symbol];

	const auto qty      = static_cast<volume_t>(lots);
	const auto notional = static_cast<std::int64_t>(price) * qty;
	const bool buying   = side == side_t::bid;

	// signed = buying ? qty : -qty, without a branch. mask is all-ones when
	// selling, so (qty ^ mask) - mask negates exactly then.
	const volume_t mask         = -static_cast<volume_t>(!buying);
	const volume_t signed_qty   = (qty ^ mask) - mask;
	const std::int64_t signed_n = (notional ^ mask) - mask;

	bump(e.net_lots, signed_qty);
	bump(e.net_notional, signed_n);
	bump(buying ? e.bought_lots : e.sold_lots, qty);
}

void position_book::add_working(symbol_id_t symbol, side_t side,
								volume_t lots) noexcept {
	assert(carries(symbol));
	bump(working(symbol, side), lots);
}

void position_book::remove_working(symbol_id_t symbol, side_t side,
								   volume_t lots) noexcept {
	assert(carries(symbol));
	std::atomic<volume_t> &slot = working(symbol, side);
	bump(slot, -lots);
	// Working quantity going negative means a retirement was applied twice,
	// or one the gate never counted. Both are ledger bugs and both make
	// every later exposure check too permissive, which is the failure a
	// risk system must not have quietly.
	assert(slot.load(std::memory_order_relaxed) >= 0 &&
		   "working quantity went negative: an order was retired twice");
}

void position_book::reset(symbol_id_t symbol) noexcept {
	assert(carries(symbol));
	entry &e = entries_[symbol];
	e.net_lots.store(0, std::memory_order_relaxed);
	e.net_notional.store(0, std::memory_order_relaxed);
	e.bought_lots.store(0, std::memory_order_relaxed);
	e.sold_lots.store(0, std::memory_order_relaxed);
	e.working_bid_lots.store(0, std::memory_order_relaxed);
	e.working_ask_lots.store(0, std::memory_order_relaxed);
}

[[nodiscard]] volume_t
position_book::net_lots(symbol_id_t symbol) const noexcept {
	assert(carries(symbol));
	return entries_[symbol].net_lots.load(std::memory_order_relaxed);
}

[[nodiscard]] volume_t position_book::working_lots(symbol_id_t symbol,
												   side_t side) const noexcept {
	assert(carries(symbol));
	const entry &e = entries_[symbol];
	return (side == side_t::bid ? e.working_bid_lots : e.working_ask_lots)
		.load(std::memory_order_relaxed);
}

[[nodiscard]] position_snapshot
position_book::snapshot(symbol_id_t symbol) const noexcept {
	assert(carries(symbol));
	const entry &e = entries_[symbol];
	return {
		.net_lots         = e.net_lots.load(std::memory_order_relaxed),
		.net_notional     = e.net_notional.load(std::memory_order_relaxed),
		.bought_lots      = e.bought_lots.load(std::memory_order_relaxed),
		.sold_lots        = e.sold_lots.load(std::memory_order_relaxed),
		.working_bid_lots = e.working_bid_lots.load(std::memory_order_relaxed),
		.working_ask_lots = e.working_ask_lots.load(std::memory_order_relaxed),
	};
}

void position_book::bump(std::atomic<volume_t> &counter,
						 volume_t delta) noexcept {
	counter.store(counter.load(std::memory_order_relaxed) + delta,
				  std::memory_order_relaxed);
}

[[nodiscard]] std::atomic<volume_t> &
position_book::working(symbol_id_t symbol, side_t side) noexcept {
	entry &e = entries_[symbol];
	return side == side_t::bid ? e.working_bid_lots : e.working_ask_lots;
}

[[nodiscard]] volume_t position_snapshot::gross_lots() const noexcept {
	const volume_t if_bids_fill = abs_of(net_lots + working_bid_lots);
	const volume_t if_asks_fill = abs_of(net_lots - working_ask_lots);
	return if_bids_fill > if_asks_fill ? if_bids_fill : if_asks_fill;
}

[[nodiscard]] std::int64_t position_snapshot::pnl(price_t mark) const noexcept {
	return net_lots * static_cast<std::int64_t>(mark) - net_notional;
}

[[nodiscard]] volume_t position_snapshot::abs_of(volume_t v) noexcept {
	const volume_t mask = v >> 63;
	return (v ^ mask) - mask;
}
} // namespace exchange::risk

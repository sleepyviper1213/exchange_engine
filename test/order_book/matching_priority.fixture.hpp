#pragma once
// Helpers for the two suites that pin how one price level is divided:
// OrderBookAllocation, which drives real matching, and OrderBookQueuePosition,
// which asks the book what it *would* do. They have to agree to the lot, so
// they build their books from the same three helpers rather than each growing
// its own.

#include "order_book.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <utility>
#include <vector>

using exchange::order_id_t;
using exchange::price_t;
using exchange::quantity_t;
using exchange::side_t;
using exchange::volume_t;

/// @brief One (id, quantity) pair to rest, in the order it should join.
using priority_quote = std::pair<order_id_t, quantity_t>;

/// @brief Rest one identified GTC order, and fail the test if it crossed.
///
/// Every queue in these suites is built out of orders that are supposed to
/// *rest*; one that trades on the way in would silently leave the level shorter
/// than the case intends, and the assertion that catches it belongs here rather
/// than in every caller.
inline void priority_rest(exchange::engine::order_book &book, order_id_t id,
						  side_t side, price_t price, quantity_t qty) {
	const std::vector<exchange::engine::trade> trades =
		book.place_order({.id = id, .side = side, .price = price, .qty = qty});
	EXPECT_TRUE(trades.empty())
		<< "order " << id << " was meant to rest, not trade";
}

/// @brief Rest @p quotes at one price, oldest first - a level with a known
/// FIFO.
inline void priority_rest_queue(exchange::engine::order_book &book, side_t side,
								price_t price,
								std::initializer_list<priority_quote> quotes) {
	for (const auto &[id, qty] : quotes)
		priority_rest(book, id, side, price, qty);
}

/// @brief Lots @p trades print against the resting order @p id, summed.
inline volume_t
priority_traded_for(const std::vector<exchange::engine::trade> &trades,
					order_id_t id) {
	volume_t lots = 0;
	for (const exchange::engine::trade &print : trades)
		if (print.resting == id) lots += print.volume;
	return lots;
}

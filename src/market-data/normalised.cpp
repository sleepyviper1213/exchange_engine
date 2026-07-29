#include "normalised.hpp"

#include "core/types.hpp"
#include "l2_book.hpp"

#include <utility>

namespace exchange::market_data {

void apply(l2_book &book, const depth_event &event) {
	for (const auto &[price, volume] : event.bids)
		book.set_level(side::bid, price, volume);
	for (const auto &[price, volume] : event.asks)
		book.set_level(side::ask, price, volume);
}

void reset(l2_book &book, book_snapshot snapshot) {
	// load() replaces a side outright, so both sides together are a full reseed
	// — no clear() first, and nothing survives from the book's previous state.
	book.load(side::bid, std::move(snapshot.bids));
	book.load(side::ask, std::move(snapshot.asks));
}

} // namespace exchange::market_data
